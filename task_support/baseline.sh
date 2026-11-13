#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
awk -f scripts/mk_bits_lut > "$b/bits_lut.h"
cc -std=c99 -O1 -D_POSIX_C_SOURCE=199309L -D_C99_SOURCE -Iinc -Isrc -Ivendor -I"$b" src/*.c task_support/baseline.c -lm -o "$b/baseline"
"$b/baseline"
