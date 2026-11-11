#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TOOLS=OFF -DBUILD_TESTS=OFF -DFFT_LIB=kissfft -DUSE_INTERNAL_AVRESAMPLE=ON
cmake --build "$build" --parallel 2
c++ -std=c++14 -Isrc tests/ordered/ordered_baseline.cpp "$build/src/libchromaprint.a" -o "$build/ordered-baseline"
"$build/ordered-baseline"
