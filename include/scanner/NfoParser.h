#pragma once
#include <QSet>
#include <QString>
#include <optional>

namespace Mc {

// Optional TMDB-sourced metadata for writeMovieNfo(), beyond the always-required
// IMDb id. Every field defaults to "unknown" (0 / empty) — a caller that only
// has the id gets the original id-only behavior.
struct NfoMovieMeta {
	int     tmdbId        = 0;       // <uniqueid type="tmdb">
	QString title;                  // <title> — localized per caller's language choice
	QString originalTitle;          // <originaltitle> — TMDB's original_title, always
	int     year           = 0;      // <year>
	QString premiered;              // <premiered> — full release date, YYYY-MM-DD
	double  voteAverage    = 0.0;    // <ratings><rating name="themoviedb">...
	int     voteCount      = 0;
};

class NfoParser {
public:
	// Canonical NFO path for a video file: same dir, same base name, .nfo extension
	static QString nfoPathFor(const QString& videoPath);

	// Read the first IMDb ID found in the NFO for this video (empty if none)
	static QString readImdbId(const QString& videoPath);

	// Result of scanning the co-named .nfo for scene-release content — see
	// scanSceneNfo(). hasArt mirrors hasSceneAsciiArt()'s old nullopt
	// semantics; text is the decoded content (empty whenever hasArt isn't
	// confirmed true).
	struct SceneNfoScan {
		std::optional<bool> hasArt;
		QString              text;
	};

	// Single read of the co-named .nfo, answering both "is this a real
	// scene-release NFO worth showing a viewer for" and, if so, its decoded
	// text — one file open instead of two, since ScanWorker previously called
	// the equivalent of this twice (once for the flag, once for the content)
	// on every scan. "Real scene-release NFO" means: as opposed to a Kodi
	// <movie> XML NFO (metadata, not release notes) or a short NFO that's
	// effectively just a bare IMDb link with nothing else in it. Covers both
	// classic CP437 box-drawing/ANSI art and the plain low-ASCII figlet-style
	// art some modern release groups use instead. hasArt is confirmed false
	// (not nullopt) if no .nfo exists there at all.
	//
	// hasArt is nullopt (rather than false) when the .nfo demonstrably exists
	// but couldn't be opened right now — locked by another process, or (this
	// library lives on a NAS share) a transient network hiccup. Callers that
	// persist this to the DB must treat nullopt as "leave the existing value
	// alone", not as a confirmed false — otherwise one flaky read during a
	// routine rescan permanently clears a badge that was correct a moment
	// earlier, which is indistinguishable from the feature just randomly
	// breaking. Only a real open+read gets to downgrade true to false.
	//
	// text is decoded from CP437 — the encoding almost all scene-release NFOs
	// use for any bytes above 0x7F (box-drawing/block-shading art), so that
	// renders correctly instead of as replacement-character garbage under
	// UTF-8; plain low-ASCII content passes through unchanged either way.
	// Note: a handful of NFOs in the wild are internally corrupted
	// (mixed/broken encoding from whatever tool wrote them) and will still
	// render as garbage no matter the decode — that's the source file, not
	// this function.
	static SceneNfoScan scanSceneNfo(const QString& videoPath);

	// Converts the modest subset of forum BBCode markup that scene/web-release
	// NFO templates commonly wrap their "GENERAL INFO" headers etc. in — [b] [i]
	// [u] [color=...] [size=N] — into HTML, so McMainWindow's viewer can render
	// them instead of showing the literal bracket tags as text. [font=...] tags
	// are stripped (we force our own monospace font regardless, for art
	// alignment); [url]/[img] become plain <a> links rather than embedded
	// images — this stays a local file viewer, it never fetches anything over
	// the network on its own. Everything else is HTML-escaped first, so this
	// is always safe to feed straight into QTextBrowser::setHtml(); malformed
	// or unrecognized tags are simply left as literal escaped text. Caller is
	// expected to wrap the result in <pre> to preserve art alignment/whitespace.
	static QString bbcodeToHtml(const QString& text);

	// Write (or update) the NFO file next to videoPath. Three cases:
	//   - Kodi-style <movie> XML already exists: only the <uniqueid type="imdb">,
	//     <uniqueid type="tmdb"> (if meta.tmdbId > 0), <id>, <title>,
	//     <originaltitle>, <year>, <premiered> and <ratings> elements are
	//     touched — one per non-empty/non-zero field in meta (any legacy
	//     <imdbid> tag is dropped); all other content is preserved.
	//   - Non-XML NFO already exists (e.g. a scene-release NFO with an IMDb
	//     id/URL embedded in free text): the id text is corrected in place —
	//     the file is never truncated/replaced. If no id-shaped token is found
	//     at all, an IMDb URL is appended rather than touching existing content.
	//     meta is ignored for this case.
	//   - No NFO exists: a minimal one is created with the id tags plus
	//     whatever meta fields the caller has. Pass a default-constructed
	//     NfoMovieMeta to fall back to the original id-only behavior, letting
	//     Kodi's own scraper fill the rest in.
	// Also registers nfoPath in the own-write suppression set so a future
	// QFileSystemWatcher callback can skip it via checkAndClearOwnWrite().
	static bool writeMovieNfo(const QString& videoPath, const QString& imdbId,
	                          const NfoMovieMeta& meta = {});

	// Extract a search-friendly title from a video filename.
	// "The.Dark.Knight.2008.BluRay.1080p.mkv" → "The Dark Knight 2008"
	static QString titleFromFilename(const QString& filename);

	// File-watcher suppression — call from the watcher's changed() slot:
	// returns true (and removes the entry) if this path was written by us,
	// so the watcher should skip re-scanning it.
	static bool checkAndClearOwnWrite(const QString& nfoPath);

private:
	static QSet<QString>& ownWrites();
};

} // namespace Mc
