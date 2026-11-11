#!/usr/bin/env bash
set -euo pipefail
src=${1:-devcheck/baseline.c}
test -s "$src"
build=$(mktemp -d ./.shine-build.XXXXXX)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -fwrapv -Isrc/lib "$src" src/lib/bitstream.c src/lib/huffman.c src/lib/l3bitstream.c src/lib/l3loop.c src/lib/l3mdct.c src/lib/l3subband.c src/lib/layer3.c src/lib/reservoir.c src/lib/tables.c -lm -o "$build/check"
"$build/check"
