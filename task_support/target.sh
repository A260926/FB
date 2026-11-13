#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/plm_impl.c
test -s tests/target_cases.c
mkdir -p .task-build
cc -std=c99 -O1 -I.  task_support/plm_impl.c tests/target_cases.c task_support/target_main.c -lm -o .task-build/target
.task-build/target
sh task_support/baseline.sh
