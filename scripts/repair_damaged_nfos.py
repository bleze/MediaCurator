"""One-off repair for scene .nfo files that an old MediaCurator version damaged
(bytes it could not read as UTF-8 were replaced by U+FFFD and written back).

For every library file with a scene NFO:
  * damaged NFO + exact srrDB release match  -> original NFO fetched, checked to be
    the same NFO, written over the damaged one (damaged copy kept in a local backup)
  * everything else                           -> file left alone
and the viewer's cached text in the database is refreshed from the bytes on disk.

Folder and file timestamps are preserved. Safe to re-run: repaired files are no
longer detected as damaged. Dry run by default; pass --apply to change anything.

    py -I scripts/repair_damaged_nfos.py [--apply] [--limit N]
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

FFFD = b"\xef\xbf\xbd"
API = "https://api.srrdb.com/v1"
UA = "MediaCurator/1.0"
DELAY_S = 1.5
APPDIR = os.path.expandvars(r"%APPDATA%\Bleze Software\MediaCurator")
DB = os.path.join(APPDIR, "mediacurator.db")
BACKUP = os.path.join(APPDIR, "nfo_repair_backup")
# NFOs picked by hand in the download dialog, where the file name is not the release name.
MANUAL_RELEASE = {2541: "Men.in.Black.II.2002.MULTI.COMPLETE.BLURAY-XORBiTANT"}
TT_RE = re.compile(rb"(?<![0-9A-Za-z])tt\d{7,8}(?![0-9A-Za-z])")


class RateLimited(Exception):
    pass


_last = 0.0


def http_get(url):
    global _last
    wait = DELAY_S - (time.time() - _last)
    if wait > 0:
        time.sleep(wait)
    _last = time.time()
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code in (429, 503):
            raise RateLimited(str(e))
        return None
    except (urllib.error.URLError, TimeoutError, OSError):
        return None


def fetch_original(release, exact_only):
    """Returns the original NFO bytes for an exact release-name match, else None."""
    if exact_only:
        body = http_get(f"{API}/search/r:{urllib.parse.quote(release, safe='')}")
        if not body:
            return None
        try:
            results = json.loads(body).get("results") or []
        except ValueError:
            return None
        hit = next((r for r in results
                    if str(r.get("release", "")).lower() == release.lower()
                    and str(r.get("hasNFO", "")).lower() == "yes"), None)
        if not hit:
            return None
        release = hit["release"]
    body = http_get(f"{API}/nfo/{urllib.parse.quote(release, safe='')}")
    if not body:
        return None
    try:
        links = json.loads(body).get("nfolink") or []
    except ValueError:
        return None
    return http_get(links[0]) if links else None


def looks_latin1(raw):
    def letter(b):
        return 65 <= b <= 90 or 97 <= b <= 122
    found, i, n = False, 0, len(raw)
    while i < n:
        if raw[i] < 0x80:
            i += 1
            continue
        s = i
        while i < n and raw[i] >= 0x80:
            if raw[i] < 0xC0:
                return False
            i += 1
        if i - s > 2:
            return False
        if not ((s > 0 and letter(raw[s - 1])) or (i < n and letter(raw[i]))):
            return False
        found = True
    return found


def decode(raw):
    """Mirror of NfoParser::decodeSceneNfoBytes. Returns (text, kind); text None = leave alone."""
    raw = raw.replace(b"\r\n", b"\n")  # the app reads in text mode
    if FFFD in raw and raw.count(0) * 20 >= len(raw):
        return None, "mixed"
    kind = "cp437"
    if any(b >= 0x80 for b in raw):
        try:
            text = raw.decode("utf-8")
            if text.startswith("﻿"):
                text = text[1:]
            kind = "utf8"
        except UnicodeDecodeError:
            if looks_latin1(raw):
                text, kind = raw.decode("latin-1"), "latin1"
            else:
                text = raw.decode("cp437")
    else:
        text = raw.decode("cp437")
    lines = re.split(r"\r\n|\r|\n", text)
    return "\n".join(re.sub(r"[ \t]+$", "", l) for l in lines), kind


def is_scene_nfo(raw):
    return len(raw) >= 200 and b"<movie" not in raw and b"<?xml" not in raw


def same_nfo(damaged, original):
    """True when `damaged` is `original` after the lossy UTF-8 round trip."""
    def norm(b):
        t = b.replace(b"\r\n", b"\n").decode("utf-8", errors="replace")
        t = re.sub("�+", "�", t)
        return [l.rstrip() for l in t.split("\n") if l.strip()]
    a, b = norm(damaged), norm(original)
    if not a or not b:
        return False
    bset = set(b)
    return sum(1 for l in a if l in bset) / len(a) >= 0.85


def release_names_in(raw):
    """Release-name-shaped tokens (Some.Movie.2012.1080p.BluRay.x264-GROUP) in the NFO text."""
    text = raw.decode("utf-8", errors="replace")
    found = re.findall(r"[A-Za-z0-9][A-Za-z0-9._'-]{8,}-[A-Za-z0-9]{2,}", text)
    out = []
    for f in found:
        if f.count(".") >= 2 and re.search(r"(19|20)\d\d|\d{3,4}p", f) and f not in out:
            out.append(f)
    return out[:3]


def carry_imdb_id(damaged, original):
    """Keep the IMDb id the app had put into the damaged file."""
    old = TT_RE.search(damaged)
    if not old:
        return original
    new = TT_RE.search(original)
    if new:
        return original[:new.start()] + old.group(0) + original[new.end():]
    nl = b"\r\n" if b"\r\n" in original else b"\n"
    out = original if original.endswith(b"\n") else original + nl
    return out + b"https://www.imdb.com/title/" + old.group(0) + b"/" + nl


def write_preserving_times(path, data):
    d = os.path.dirname(path)
    ds, fs = os.stat(d), os.stat(path)
    with open(path, "wb") as f:
        f.write(data)
    os.utime(path, ns=(fs.st_atime_ns, fs.st_mtime_ns))
    os.utime(d, ns=(ds.st_atime_ns, ds.st_mtime_ns))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--limit", type=int, default=0, help="stop after N srrDB lookups")
    args = ap.parse_args()
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    mode = "rw" if args.apply else "ro"
    con = sqlite3.connect("file:" + DB.replace("\\", "/") + "?mode=" + mode, uri=True, timeout=30)
    rows = con.execute("SELECT id, path, scene_nfo_text FROM files WHERE has_scene_nfo=1").fetchall()
    if args.apply:
        os.makedirs(BACKUP, exist_ok=True)

    stats = dict(total=len(rows), unreadable=0, damaged=0, repaired=0, no_match=0,
                 mismatch=0, cache_refreshed=0)
    lookups = 0
    stopped = None
    for n, (fid, path, cached) in enumerate(rows, 1):
        nfo = os.path.splitext(path)[0] + ".nfo"
        try:
            with open(nfo, "rb") as f:
                raw = f.read()
        except OSError:
            stats["unreadable"] += 1
            continue
        if not is_scene_nfo(raw):
            continue
        name = os.path.basename(os.path.splitext(path)[0])

        damaged = FFFD in raw and raw.count(0) * 20 < len(raw)
        if damaged:
            stats["damaged"] += 1
            if stopped or (args.limit and lookups >= args.limit):
                continue
            lookups += 1
            # The NFO on disk is not always the one for this file name (hand-picked,
            # or the video was renamed), but it usually names its own release - so
            # try those names first, then the file name. Whatever is found is only
            # accepted if its content matches the damaged file (same_nfo).
            candidates = []
            for c in ([MANUAL_RELEASE[fid]] if fid in MANUAL_RELEASE else []) + release_names_in(raw) + [name]:
                if c.lower() not in (x.lower() for x in candidates):
                    candidates.append(c)
            original, found, saw_other = None, None, False
            try:
                for c in candidates[:4]:
                    got = fetch_original(c, exact_only=True)
                    if not got:
                        continue
                    if FFFD not in got and same_nfo(raw, got):
                        original, found = got, c
                        break
                    saw_other = True
            except RateLimited as e:
                stopped = f"srrDB rate limit ({e}) - re-run later to continue"
                print("!!", stopped, flush=True)
                continue
            if not original:
                stats["mismatch" if saw_other else "no_match"] += 1
                why = "srrDB has a different NFO" if saw_other else "no srrDB match"
                print(f"[{n}/{len(rows)}] {why}   {name}", flush=True)
                continue
            fixed = carry_imdb_id(raw, original)
            print(f"[{n}/{len(rows)}] repair   {name}   <- {found}", flush=True)
            stats["repaired"] += 1
            if args.apply:
                with open(os.path.join(BACKUP, f"{fid}_{os.path.basename(nfo)}"), "wb") as f:
                    f.write(raw)
                write_preserving_times(nfo, fixed)
            raw = fixed

        text, kind = decode(raw)
        if text is None or text == cached:
            continue
        # Undamaged CP437 files are what the cache already holds; only touch rows
        # whose bytes on disk are known to decode differently now.
        if not damaged and kind == "cp437":
            continue
        stats["cache_refreshed"] += 1
        if args.apply:
            con.execute("UPDATE files SET scene_nfo_text=? WHERE id=?", (text, fid))
            con.commit()

    print(("APPLIED " if args.apply else "DRY RUN ") + json.dumps(stats))
    if args.apply:
        print("damaged originals backed up to", BACKUP)


if __name__ == "__main__":
    main()
