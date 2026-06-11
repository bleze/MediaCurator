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
MKV_VERSION="88.0"

case "$(uname)" in
    Darwin)
        PLATFORM="macos"
        # evermeet.cx: standalone static ffprobe for macOS, latest release alias
        FFPROBE_URL="https://evermeet.cx/ffmpeg/getrelease/ffprobe/zip"
        MKV_URL="https://mkvtoolnix.download/macos/MKVToolNix-${MKV_VERSION}.0.dmg"
        ;;
    *)
        PLATFORM="linux"
        # johnvansickle.com: static ffprobe build for amd64
        FFPROBE_URL="https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-amd64-static.tar.xz"
        MKV_URL="https://mkvtoolnix.download/appimage/MKVToolNix_GUI-${MKV_VERSION}.0-x86_64.AppImage"
        ;;
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
        echo "  Downloading from evermeet.cx..."
        TMP_ZIP=$(mktemp /tmp/ffprobe-XXXXXX.zip)
        curl -fsSL "$FFPROBE_URL" -o "$TMP_ZIP"
        unzip -o -j "$TMP_ZIP" "ffprobe" -d "$TOOLS_DIR"
        rm -f "$TMP_ZIP"
    else
        echo "  Downloading from johnvansickle.com..."
        TMP_TAR=$(mktemp /tmp/ffprobe-XXXXXX.tar.xz)
        TMP_DIR=$(mktemp -d /tmp/ffprobe-XXXXXX)
        curl -fsSL "$FFPROBE_URL" -o "$TMP_TAR"
        tar -xf "$TMP_TAR" -C "$TMP_DIR" --strip-components=1
        cp "$TMP_DIR/ffprobe" "$FFPROBE_BIN"
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
        echo "  Downloading DMG from mkvtoolnix.download..."
        TMP_DMG=$(mktemp /tmp/mkvtoolnix-XXXXXX.dmg)
        TMP_MOUNT=$(mktemp -d /tmp/mkvtoolnix-mount-XXXXXX)
        curl -fsSL "$MKV_URL" -o "$TMP_DMG"

        echo "  Mounting DMG..."
        hdiutil attach -nobrowse -noverify -quiet "$TMP_DMG" -mountpoint "$TMP_MOUNT"

        APP_CONTENTS="$TMP_MOUNT/MKVToolNix.app/Contents"
        for BIN_NAME in mkvmerge mkvextract mkvpropedit mkvinfo; do
            [[ -f "$APP_CONTENTS/MacOS/$BIN_NAME" ]] && \
                cp "$APP_CONTENTS/MacOS/$BIN_NAME" "$MKV_DIR/" && \
                chmod +x "$MKV_DIR/$BIN_NAME"
        done
        # Frameworks sit one level above mkvtoolnix/ so that mkvmerge's
        # @executable_path/../Frameworks rpath resolves correctly at runtime.
        [[ -d "$APP_CONTENTS/Frameworks" ]] && \
            cp -R "$APP_CONTENTS/Frameworks" "$TOOLS_DIR/"

        hdiutil detach "$TMP_MOUNT" -quiet
        rm -rf "$TMP_DMG" "$TMP_MOUNT"

    else
        echo "  Downloading AppImage from mkvtoolnix.download..."
        TMP_IMG=$(mktemp /tmp/mkvtoolnix-XXXXXX.AppImage)
        TMP_DIR=$(mktemp -d /tmp/mkvtoolnix-XXXXXX)
        curl -fsSL "$MKV_URL" -o "$TMP_IMG"
        chmod +x "$TMP_IMG"

        echo "  Extracting AppImage..."
        (cd "$TMP_DIR" && "$TMP_IMG" --appimage-extract > /dev/null 2>&1)

        for BIN_NAME in mkvmerge mkvextract mkvpropedit mkvinfo; do
            SRC=$(find "$TMP_DIR/squashfs-root" -name "$BIN_NAME" -not -path "*/lib/*" -type f | head -1)
            if [[ -n "$SRC" ]]; then
                cp "$SRC" "$MKV_DIR/"
                chmod +x "$MKV_DIR/$BIN_NAME"
            fi
        done
        # Copy bundled Qt + support libs; patchelf points the binaries at $ORIGIN/lib.
        LIB_SRC=$(find "$TMP_DIR/squashfs-root" -maxdepth 5 -name "libQt6Core.so*" -type f | head -1)
        if [[ -n "$LIB_SRC" ]]; then
            cp -r "$(dirname "$LIB_SRC")" "$MKV_DIR/lib"
            if command -v patchelf &>/dev/null; then
                for BIN in "$MKV_DIR"/mkv*; do
                    patchelf --set-rpath '$ORIGIN/lib' "$BIN" 2>/dev/null || true
                done
            fi
        fi

        rm -rf "$TMP_DIR" "$TMP_IMG"
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
  "ffprobe":    { "url": "$FFPROBE_URL" },
  "mkvtoolnix": { "version": "$MKV_VERSION", "url": "$MKV_URL" }
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
