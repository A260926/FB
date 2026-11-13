#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build" -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF >"$build/configure.log" 2>&1
cmake --build "$build" --target faad -j 4 >"$build/build.log" 2>&1
cc -std=c99 -DHAVE_INTTYPES_H=1 -DHAVE_LRINTF=1 -Iinclude -Ilibfaad support/baseline.c "$build/libfaad.a" -lm -o "$build/baseline"
"$build/baseline"
