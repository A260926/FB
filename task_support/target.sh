#!/bin/sh
set -eu
bash task_support/baseline.sh
for f in minimp3_push.c minimp3_push.h tests/push_decode_test.c; do
 [ -s "$f" ] || { echo "missing $f" >&2; exit 2; }
done
cc -std=c99 -O2 -Wall -Wextra -I. minimp3_push.c tests/push_decode_test.c -lm -o .task-build/push-test
.task-build/push-test > .task-build/push.log
cat .task-build/push.log
grep -Eq '^MINIMP3_PUSH_OK cases=[1-9][0-9]*$' .task-build/push.log
