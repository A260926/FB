#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh devcheck/build.sh
"${CC:-cc}" -std=c99 -O2 -DHAVE_CONFIG_H -include devcheck/include/config.h -Idevcheck/include -Ilibtwolame -Iinclude -Ilibspeex tests/frame_budget.c .build/libcodec.a -lm -o .build/target
.build/target
.build/baseline
