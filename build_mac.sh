#!/usr/bin/env bash
# Native macOS build. Produces build-mac/bin/doom (+ assets/doom1.wad beside it).
set -e
cd "$(dirname "$0")"
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release
cmake --build build-mac -j
echo
echo "Built: $(pwd)/build-mac/bin/doom"
echo "Run:   (cd build-mac/bin && ./doom)"
