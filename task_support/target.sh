#!/bin/sh
set -eu
bash task_support/baseline.sh
[ -s tests/pcm24_test.c ] || { echo 'missing tests/pcm24_test.c' >&2; exit 2; }
cc -std=c99 -O2 -Wall -Wextra -Isrc tests/pcm24_test.c .task-build/lib/src/libsoxr.a -lm -o .task-build/pcm24-test
.task-build/pcm24-test > .task-build/pcm24.log
cat .task-build/pcm24.log
grep -Eq '^SOXR_PCM24_OK cases=[1-9][0-9]*$' .task-build/pcm24.log
