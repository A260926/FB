#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
sources=(src/lib/bitstream.c src/lib/huffman.c src/lib/l3bitstream.c src/lib/l3loop.c src/lib/l3mdct.c src/lib/l3subband.c src/lib/layer3.c src/lib/reservoir.c src/lib/tables.c)
"${CC:-cc}" -std=c99 -O2 -fsigned-char -Isrc/lib "${sources[@]}" devcheck/baseline.c -lm -o .build/baseline
"${CC:-cc}" -std=c99 -O2 -fsigned-char -Isrc/lib "${sources[@]}" src/bin/main.c src/bin/wave.c -lm -o .build/shineenc
