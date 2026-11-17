#!/usr/bin/env bash
# Prints the GitHub release notes for Hot DAMN NFO Viewer VERSION: its section of
# apps/hotdamn/CHANGELOG.md plus the download note. Fails if there is no section.
# Used by hotdamn.yml (new releases) and release-metadata.yml (existing ones).
set -euo pipefail
version="$1"
section=$(awk -v v="$version" '{ sub(/\r$/, "") } $0 == "## " v { f = 1; next } /^## / && f { exit } f' apps/hotdamn/CHANGELOG.md)
if ! grep -q '[^[:space:]]' <<< "$section"; then
  echo "apps/hotdamn/CHANGELOG.md has no section '## ${version}'" >&2
  exit 1
fi
printf '%s\n\n---\n' "$section"
printf 'Download `HotDamn-Setup-%s.exe` below and run it (Windows 10/11, 64-bit). ' "$version"
printf 'The installer is not code-signed, so Windows SmartScreen may ask you to confirm: **More info → Run anyway**.\n\n'
printf "This is not a MediaCurator release; MediaCurator's own downloads are under [Latest release](https://github.com/bleze/MediaCurator/releases/latest).\n"
