#!/usr/bin/env bash
# Stamp the build version into the web output: replaces the __DOOM_VERSION__ token
# in the generated HTML + service worker, and writes version.txt.
#   $1 = bin/output dir, $2 = source dir (for git)
set -e
BIN="$1"; SRC="$2"

HASH="$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo local)"
DIRTY=""
git -C "$SRC" diff --quiet 2>/dev/null || DIRTY="+"
VER="$(date +%Y%m%d-%H%M%S)-${HASH}${DIRTY}"

for f in index.html doom.html sw.js; do
    [ -f "$BIN/$f" ] && perl -pi -e "s/__DOOM_VERSION__/$VER/g" "$BIN/$f"
done
printf '%s\n' "$VER" > "$BIN/version.txt"
echo "DOOM web version: $VER"
