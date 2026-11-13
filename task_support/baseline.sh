#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
sh task_support/build.sh
cc -std=c99 -O1 -g ${CHECK_SANITIZERS:-} -Iinclude -I.task-build/include task_support/baseline.c .task-build/lib/libevent_core.a -o .task-build/baseline
.task-build/baseline "$@"
