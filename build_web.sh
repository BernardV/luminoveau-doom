#!/usr/bin/env bash
# WebAssembly build via Emscripten. Produces build-web/bin/doom.html (+ .wasm,
# .js, .data). Serve the folder over HTTP and open doom.html in a WebGPU browser.
set -e
cd "$(dirname "$0")"

# Locate emscripten (brew keg) and put its tools + a node on PATH.
EMDIR="$(brew --prefix emscripten)/libexec"
[ -f "$EMDIR/emcc" ] || EMDIR="$(cd "$(dirname "$(readlink "$(brew --prefix emscripten)/libexec/emcc" 2>/dev/null || echo "$EMDIR/emcc")")" && pwd)"
export PATH="$EMDIR:$PATH"
command -v node >/dev/null || export PATH="$(dirname "$(command -v node || true)"):$PATH"

emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web -j

echo
echo "Built: $(pwd)/build-web/bin/doom.html"
echo "Serve: (cd build-web/bin && python3 -m http.server 8000) then open http://localhost:8000/doom.html"
