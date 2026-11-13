#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cc -std=c99 -O2 -Itask_support task_support/baseline.c -Iinclude -I.task-build -I.task-build/include $(find .task-build -name libcares.a -print -quit) -o .task-build/task_baseline
.task-build/task_baseline
