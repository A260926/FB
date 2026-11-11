#!/bin/sh
# Runs checks/overlap_contract.c twice under TIGR_HEADLESS, without opening
# any window:
#   1. linked against the release single-file build (tigr.c)
#   2. linked against the modular maintenance source (src/tigr_bitmaps.c)
# Both builds must satisfy the same in-place blit assertions.
set -eu

cd "$(dirname "$0")/.."
mkdir -p .build

echo "== release amalgamation: tigr.c =="
cc -std=c99 -Wall -Wextra -pedantic -DTIGR_HEADLESS -I. \
    checks/overlap_contract.c tigr.c -lm -o .build/overlap-release
.build/overlap-release

echo "== modular maintenance source: src/tigr_bitmaps.c =="
cc -std=c99 -Wall -Wextra -pedantic -DTIGR_HEADLESS -I. -include tigr.h \
    checks/overlap_contract.c src/tigr_bitmaps.c -lm -o .build/overlap-modular
.build/overlap-modular

echo "OVERLAP CONTRACT OK (both builds)"
