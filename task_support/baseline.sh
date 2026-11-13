#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh task_support/build.sh
cc -std=gnu99 -O1 -g ${CHECK_SANITIZERS:-} -Itask_support -Ilib -I.task-build task_support/baseline.c .task-build/libtar.a -o .task-build/baseline
.task-build/baseline
