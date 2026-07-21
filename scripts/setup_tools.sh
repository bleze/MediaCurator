#!/usr/bin/env bash
#
# Downloads and stages ffprobe and mkvtoolnix into tools/<platform>/.
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

# Pinned versions - keep in sync with setup_tools.ps1
MKV_VERSION="100.0"

case "$(uname)" in
    Darwin) PLATFORM="macos" ;;
    *)      PLATFORM="linux" ;;
esac

TOOLS_DIR="$ROOT/tools/$PLATFORM"
FFPROBE_BIN="$TOOLS_DIR/ffprobe"
MKV_DIR="$TOOLS_DIR/mkvtoolnix"
MKV_BIN="$MKV_DIR/mkvmerge"

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
        curl -fsSL "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz" \
            -o "$TMP_TAR"
        # BtbN archive layout: ffmpeg-master-latest-linux64-gpl/bin/{ffmpeg,ffprobe,...}
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
if [[ ! -f "$MKV_BIN" ]] || $FORCE; then
    mkdir -p "$MKV_DIR"
    echo "[mkvtoolnix $MKV_VERSION]"

    if [[ "$PLATFORM" == "macos" ]]; then
        echo "  Installing via Homebrew..."
        brew install --quiet mkvtoolnix
        BREW_BIN="$(brew --prefix mkvtoolnix)/bin"
        for BIN_NAME in mkvmerge mkvextract mkvpropedit mkvinfo; do
            [[ -f "$BREW_BIN/$BIN_NAME" ]] && \
                cp "$BREW_BIN/$BIN_NAME" "$MKV_DIR/" && \
                chmod +x "$MKV_DIR/$BIN_NAME"
        done
    else
        echo "  Installing via apt..."
        sudo apt-get install -y --no-install-recommends mkvtoolnix
        for BIN_NAME in mkvmerge mkvextract mkvpropedit mkvinfo; do
            BIN_PATH=$(command -v "$BIN_NAME" 2>/dev/null || true)
            [[ -n "$BIN_PATH" ]] && cp "$BIN_PATH" "$MKV_DIR/" && chmod +x "$MKV_DIR/$BIN_NAME"
        done
    fi

    echo "  OK"
else
    echo "[mkvtoolnix] already present  (use --force to re-download)"
fi

# ─────────────────────────────────────────────────────────────────────────────
# 3. Version manifest
# ─────────────────────────────────────────────────────────────────────────────
cat > "$TOOLS_DIR/versions.json" <<EOF
{
  "ffprobe":    { "source": "$(if [[ $PLATFORM == macos ]]; then echo brew; else echo btbn-github; fi)" },
  "mkvtoolnix": { "version": "$MKV_VERSION" }
}
EOF

# ─────────────────────────────────────────────────────────────────────────────
# 4. Status
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "Status:"
for BIN_PATH in "$FFPROBE_BIN" "$MKV_BIN"; do
    LABEL="${BIN_PATH#$ROOT/}"
    if [[ -f "$BIN_PATH" ]]; then
        echo "  [OK]      $LABEL"
    else
        echo "  [MISSING] $LABEL"
    fi
done

echo ""
echo "To upgrade tools, bump the version variables at the top of this script,"
echo "then re-run with --force."
