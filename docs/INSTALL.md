# Installing MediaCurator

Pre-built installers for each release are attached to the
[GitHub Releases](https://github.com/bleze/MediaCurator/releases) page.

---

## Windows

**Requirement:** Windows 10 22H2 or later (64-bit)

1. Download `MediaCurator-<version>-win64.exe` from the releases page.
2. Run the installer and follow the wizard.
   - By default installs to `C:\Program Files\MediaCurator\`.
   - Adds a Start Menu shortcut.
   - The uninstaller is registered under *Add or Remove Programs*.
3. On first launch, go to **Settings → Tools** and point MediaCurator at your
   `ffprobe.exe` and `mkvmerge.exe` if they are not bundled in the `tools\`
   sub-folder next to the executable.

> The installer bundles all required Qt 6 runtime DLLs — no separate Qt
> installation is needed.

---

## macOS

**Requirement:** macOS 12 Monterey or later

1. Download `MediaCurator-<version>-Darwin.dmg` from the releases page.
2. Open the `.dmg`, drag **MediaCurator.app** into your **Applications** folder.
3. On first launch macOS may show a Gatekeeper warning (app is not notarized).
   To allow it: *System Settings → Privacy & Security → Open Anyway*.

> The `.app` bundle includes all Qt 6 frameworks — no separate Qt installation
> is needed.

---

## Linux (Debian / Ubuntu)

**Requirement:** Ubuntu 22.04 LTS or later, or any Debian-based distro with Qt 6 packages

Install Qt 6 runtime dependencies first (one-time):

```bash
sudo apt-get install -y \
  libqt6core6t64 libqt6gui6t64 libqt6widgets6t64 \
  libqt6sql6t64 libqt6network6t64 libqt6svg6 \
  libqt6concurrent6t64
```

> On Ubuntu 22.04 the package names do not have the `t64` suffix — use
> `libqt6core6`, `libqt6gui6`, etc. instead.

Then install the `.deb`:

```bash
sudo dpkg -i MediaCurator-<version>-Linux.deb
```

Launch with:

```bash
MediaCurator
```

---

## Building from source

See [README.md](../README.md) for full build instructions. In short:

```bash
# Requires: CMake 3.25+, Qt 6.8+, MSVC / Clang / GCC

cmake --preset local-release          # Windows (adjust CMakeUserPresets.json for Qt path)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DQt6_DIR=/path/to/Qt/6.8.3/.../lib/cmake/Qt6
cmake --build build --parallel
```

---

## External tools (ffprobe + mkvmerge + dovi_tool)

MediaCurator calls `ffprobe` (for scanning), `mkvmerge`/`mkvextract` (for
remuxing and the Dolby Vision deep scan), and `dovi_tool` (for the Dolby
Vision deep scan only) as external processes. `ffprobe` and `mkvmerge` are
**not** bundled in the installer because of licensing constraints (LGPL /
GPL); `dovi_tool` is MIT-licensed, so it carries no such restriction.

Place them in the `tools/` directory next to the executable, or configure
their paths in **Settings → Tools**:

| Tool | Download |
|------|---------|
| `ffprobe` | [ffmpeg.org/download.html](https://ffmpeg.org/download.html) — take any static build, copy just `ffprobe` |
| `mkvmerge` / `mkvextract` | [mkvtoolnix.download](https://mkvtoolnix.download/downloads.html) — install MKVToolNix and copy both |
| `dovi_tool` | [github.com/quietvoid/dovi_tool/releases](https://github.com/quietvoid/dovi_tool/releases) — only needed for the opt-in "Deep Scan for FEL/MEL" action |
