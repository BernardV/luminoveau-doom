#!/usr/bin/env bash
# Idempotently apply our local Luminoveau patches to the FetchContent-fetched
# engine source. Called from CMake's PATCH_COMMAND with the patch files as args.
#
# It runs inside the fetched engine repo (its root). For each patch: if it's
# already applied (reverse-check succeeds) it's skipped, otherwise it's applied —
# so a re-configure without a fresh clone won't error out on already-patched files.
set -e
for p in "$@"; do
    if git apply --reverse --check "$p" >/dev/null 2>&1; then
        echo "luminoveau patch already applied: $(basename "$p")"
    else
        echo "applying luminoveau patch: $(basename "$p")"
        git apply "$p"
    fi
done
