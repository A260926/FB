#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c99 -O2 -Itask_support task_support/baseline.c -I. -I.task-build .task-build/libz.a -o .task-build/task_baseline
.task-build/task_baseline
