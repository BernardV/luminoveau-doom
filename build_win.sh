#!/usr/bin/env bash
# Cross-compile a Windows x86_64 build from macOS/Linux using mingw-w64.
# Produces build-win/bin/ : doom.exe + runtime DLLs + assets/ (a runnable folder).
#
# Requires: mingw-w64 (brew install mingw-w64) and cmake.
# NOTE: built with RelWithDebInfo on purpose — the engine hardcodes
# -march=native in its Release CXX flags, which mingw's gcc rejects.
set -e
cd "$(dirname "$0")"

MW="$(dirname "$(command -v x86_64-w64-mingw32-gcc)")/../x86_64-w64-mingw32"

cmake -S . -B build-win \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake
cmake --build build-win -j

BIN=build-win/bin

# Bundle the mingw runtime DLLs the exe imports.
for d in libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    f="$(find "$MW/.." -name "$d" 2>/dev/null | head -1)"
    [ -n "$f" ] && cp "$f" "$BIN/"
done

# Strip debug info (185 MB -> ~12 MB).
x86_64-w64-mingw32-strip "$BIN/doom.exe" || true
rm -f "$BIN/.DS_Store"

echo
echo "Windows build ready: $(pwd)/$BIN"
echo "  Copy the whole '$BIN' folder to a Windows machine and run doom.exe."
