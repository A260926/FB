#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
"${CC:-cc}" -std=c99 -O2 -Idevcheck/include -Ilibtwolame devcheck/dab_gap.c .build/libcodec.a -lm -o .build/dab-gap
.build/dab-gap
