#!/bin/sh
set -eu
bash task_support/build.sh
cc -std=c99 -O2 -Isrc task_support/baseline.c .task-build/lib/src/libsoxr.a -lm -o .task-build/baseline
.task-build/baseline
ctest --test-dir .task-build/lib --output-on-failure
