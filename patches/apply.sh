#!/usr/bin/env bash
# Apply our local Luminoveau patches to the FetchContent-fetched engine source.
# Called from CMake's PATCH_COMMAND with the patch files as args; runs inside the
# fetched engine repo (its root).
#
# It first resets the tree to the pinned ref, then applies every patch fresh. That
# makes it robust to a stale build dir: if a previous build applied an older version
# of a patch, a plain re-apply would conflict ("patch does not apply"). Resetting
# first guarantees a clean base every time — idempotent across patch changes.
set -e

if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git checkout -- . 2>/dev/null || true    # discard previously-applied patches
fi

for p in "$@"; do
    if git apply --reverse --check "$p" >/dev/null 2>&1; then
        echo "luminoveau patch already applied: $(basename "$p")"
    else
        echo "applying luminoveau patch: $(basename "$p")"
        git apply "$p"
    fi
done
