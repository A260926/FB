#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s h265_stream.c
test -s task_support/minimp4_impl.c
test -s tests/target_cases.c
mkdir -p .task-build
cc -std=c99 -O1 -I.  h265_stream.c task_support/minimp4_impl.c tests/target_cases.c task_support/target_main.c -o .task-build/target
.task-build/target
sh task_support/baseline.sh
