#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh devcheck/build.sh
"${CC:-cc}" -std=c99 -O2 -Idevcheck/include -Ilibtwolame tests/dab_stream.c .build/libcodec.a -lm -o .build/dab-stream
.build/dab-stream
"${CC:-cc}" -std=c99 -O2 -Idevcheck/include -Ilibtwolame examples/dab_stream.c .build/libcodec.a -lm -o .build/dab-example
.build/dab-example .build/dab-example.mp2
.build/baseline
sh devcheck/dab_gap.sh
