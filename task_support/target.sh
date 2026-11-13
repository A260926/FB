#!/bin/sh
set -eu
bash task_support/baseline.sh
f=tests/speed_track_test.c
[ -s "$f" ] || { echo "missing $f" >&2; exit 2; }
cc -std=c99 -O2 -Wall -Wextra -I. sonic.c "$f" -lm -o .task-build/speed-track
.task-build/speed-track > .task-build/speed-track.log
cat .task-build/speed-track.log
grep -Eq '^SONIC_SPEED_TRACK_OK cases=[1-9][0-9]*$' .task-build/speed-track.log
