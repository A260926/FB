#!/bin/sh
set -eu
bash support/build.sh
cc -std=c99 -Isrc support/baseline.c .build/libltc.a -lm -o .build/baseline
.build/baseline
cc -std=c99 -Isrc tests/ltcloop.c .build/libltc.a -lm -o .build/ltcloop
.build/ltcloop
