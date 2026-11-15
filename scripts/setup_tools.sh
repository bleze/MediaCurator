#!/usr/bin/env bash
#
# Downloads and stages ffprobe, mkvtoolnix, and dovi_tool into tools/<platform>/.
# Run once after cloning, and whenever PINNED_VERSIONS changes.
#
# Usage:
#   bash scripts/setup_tools.sh [--force]
#
# --force  Re-download even if the tools are already present.

set -euo pipefail

FORCE=false
[[ "${1:-}" == "--force" ]] && FORCE=true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Pinned versions - keep in sync with setup_tools.ps1.
# mkvtoolnix is not pinned here: it comes from Homebrew (macOS) or apt (Linux),
# which install whatever version they currently carry.
DOVI_TOOL_VERSION="2.3.4"

# Linux ffprobe: BtbN's LGPL build of this FFmpeg release branch (same major
# line and licence as the Windows gyan.dev build). The asset is rebuilt as the
# branch gets point releases, so this pins the branch, not an exact patch.
FFMPEG_BRANCH="9.0"

case "$(uname)" in
    Darwin) PLATFORM="macos" ;;
    *)      PLATFORM="linux" ;;
esac

TOOLS_DIR="$ROOT/tools/$PLATFORM"
FFPROBE_BIN="$TOOLS_DIR/ffprobe"
MKV_DIR="$TOOLS_DIR/mkvtoolnix"
MKV_BIN="$MKV_DIR/mkvmerge"
DOVI_DIR="$TOOLS_DIR/dovi_tool"
DOVI_BIN="$DOVI_DIR/dovi_tool"

DOVI_BASE_URL="https://github.com/quietvoid/dovi_tool/releases/download/$DOVI_TOOL_VERSION"
if [[ "$PLATFORM" == "macos" ]]; then
    DOVI_URL="$DOVI_BASE_URL/dovi_tool-$DOVI_TOOL_VERSION-universal-macOS.zip"
else
    case "$(uname -m)" in
        aarch64|arm64) DOVI_ARCH="aarch64" ;;
        *)             DOVI_ARCH="x86_64" ;;
    esac
    DOVI_URL="$DOVI_BASE_URL/dovi_tool-$DOVI_TOOL_VERSION-$DOVI_ARCH-unknown-linux-musl.tar.gz"
fi

mkdir -p "$TOOLS_DIR"

echo "MediaCurator - tool setup ($PLATFORM)"
echo "  Target: $TOOLS_DIR"
echo ""

# ─────────────────────────────────────────────────────────────────────────────
# 1. ffprobe
# ─────────────────────────────────────────────────────────────────────────────
if [[ ! -f "$FFPROBE_BIN" ]] || $FORCE; then
    echo "[ffprobe]"

    if [[ "$PLATFORM" == "macos" ]]; then
        echo "  Installing via Homebrew..."
        brew install --quiet ffmpeg
        cp "$(brew --prefix ffmpeg)/bin/ffprobe" "$FFPROBE_BIN"
    else
        echo "  Downloading from BtbN/FFmpeg-Builds (GitHub)..."
        TMP_TAR=$(mktemp /tmp/ffprobe-XXXXXX.tar.xz)
        TMP_DIR=$(mktemp -d /tmp/ffprobe-XXXXXX)
        curl -fsSL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n${FFMPEG_BRANCH}-latest-linux64-lgpl-${FFMPEG_BRANCH}.tar.xz" \
            -o "$TMP_TAR"
        # BtbN archive layout: ffmpeg-n<branch>-latest-linux64-lgpl-<branch>/bin/{ffmpeg,ffprobe,...}
        tar -xf "$TMP_TAR" -C "$TMP_DIR" --strip-components=1
        cp "$TMP_DIR/bin/ffprobe" "$FFPROBE_BIN"
        rm -rf "$TMP_TAR" "$TMP_DIR"
    fi

    chmod +x "$FFPROBE_BIN"
    echo "  OK"
else
    echo "[ffprobe] already present  (use --force to re-download)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 2. mkvtoolnix
# ─────────────────────────────────────────────────────────────────────────────
if [[ "$PLATFORM" == "macos" ]]; then
    if [[ ! -f "$MKV_BIN" ]] || $FORCE; then
        mkdir -p "$MKV_DIR"
        echo "[mkvtoolnix]"
        echo "  Installing via Homebrew..."
        brew install --quiet mkvtoolnix
        BREW_BIN="$(brew --prefix mkvtoolnix)/bin"
        for BIN_NAME in mkvmerge mkvextract mkvpropedit mkvinfo; do
            [[ -f "$BREW_BIN/$BIN_NAME" ]] && \
                cp "$BREW_BIN/$BIN_NAME" "$MKV_DIR/" && \
                chmod +x "$MKV_DIR/$BIN_NAME"
        done
        echo "  OK"
    else
        echo "[mkvtoolnix] already present  (use --force to re-download)"
    fi
else
    # Not bundled on Linux: the distro's mkvmerge is dynamically linked against
    # that distro's own boost/libebml/Qt, so a copy only runs on the release it
    # came from. The .deb declares "Depends: mkvtoolnix" instead (see
    # cmake/Packaging.cmake) and ExternalTools falls back to PATH.
    rm -rf "$MKV_DIR"
    if ! command -v mkvmerge >/dev/null 2>&1 || $FORCE; then
        echo "[mkvtoolnix]"
        echo "  Installing via apt..."
        sudo apt-get install -y --no-install-recommends mkvtoolnix
        echo "  OK"
    else
        echo "[mkvtoolnix] already installed system-wide"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# 3. dovi_tool (MIT) - powers the opt-in "Deep Scan for FEL/MEL" action only.
#    Single static binary on both platforms (musl on Linux, universal on macOS).
# ─────────────────────────────────────────────────────────────────────────────
if [[ ! -f "$DOVI_BIN" ]] || $FORCE; then
    mkdir -p "$DOVI_DIR"
    echo "[dovi_tool $DOVI_TOOL_VERSION]"
    echo "  Downloading from quietvoid/dovi_tool (GitHub)..."

    TMP_DIR=$(mktemp -d /tmp/dovi_tool-XXXXXX)
    TMP_ARCHIVE="$TMP_DIR/${DOVI_URL##*/}"
    curl -fsSL "$DOVI_URL" -o "$TMP_ARCHIVE"
    # Both archives contain just the dovi_tool binary at the top level.
    if [[ "$TMP_ARCHIVE" == *.zip ]]; then
        unzip -q -o "$TMP_ARCHIVE" -d "$TMP_DIR"
    else
        tar -xzf "$TMP_ARCHIVE" -C "$TMP_DIR"
    fi
    cp "$TMP_DIR/dovi_tool" "$DOVI_BIN"
    rm -rf "$TMP_DIR"

    chmod +x "$DOVI_BIN"
    echo "  OK"
else
    echo "[dovi_tool] already present  (use --force to re-download)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 4. Version manifest
# ─────────────────────────────────────────────────────────────────────────────
if [[ "$PLATFORM" == "macos" ]]; then
    FFPROBE_MANIFEST='{ "source": "brew" }'
    MKV_MANIFEST='{ "source": "brew" }'
else
    FFPROBE_MANIFEST="{ \"source\": \"btbn-github-lgpl\", \"branch\": \"$FFMPEG_BRANCH\" }"
    MKV_MANIFEST='{ "source": "apt", "bundled": false }'
fi

cat > "$TOOLS_DIR/versions.json" <<EOF
{
  "ffprobe":    $FFPROBE_MANIFEST,
  "mkvtoolnix": $MKV_MANIFEST,
  "dovi_tool":  { "version": "$DOVI_TOOL_VERSION", "url": "$DOVI_URL" }
}
EOF

# ─────────────────────────────────────────────────────────────────────────────
# 5. Status
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "Status:"
CHECKS=("$FFPROBE_BIN" "$DOVI_BIN")
[[ "$PLATFORM" == "macos" ]] && CHECKS+=("$MKV_BIN")
for BIN_PATH in "${CHECKS[@]}"; do
    LABEL="${BIN_PATH#$ROOT/}"
    if [[ -f "$BIN_PATH" ]]; then
        echo "  [OK]      $LABEL"
    else
        echo "  [MISSING] $LABEL"
    fi
done
if [[ "$PLATFORM" == "linux" ]]; then
    if command -v mkvmerge >/dev/null 2>&1; then
        echo "  [OK]      mkvmerge (system: $(command -v mkvmerge))"
    else
        echo "  [MISSING] mkvmerge (system)"
    fi
fi

echo ""
echo "To upgrade tools, bump the version variables at the top of this script,"
echo "then re-run with --force."
