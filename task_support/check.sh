#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s task_support/target.c
sh task_support/baseline.sh
cc -std=c99 -O2 -Itask_support task_support/driver.c task_support/target.c -Iinclude -I.task-build -I.task-build/include $(find .task-build -name libcares.a -print -quit) -o .task-build/task_target
.task-build/task_target
