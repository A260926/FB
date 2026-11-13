#!/bin/sh
set -eu
test -s tests/stream_encoder.c || { echo 'missing tests/stream_encoder.c'; exit 2; }
bash support/build.sh
cc -std=c99 -Isrc -Isupport support/test_main.c tests/stream_encoder.c .build/libltc.a -lm -o .build/stream-tests
.build/stream-tests
