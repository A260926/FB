#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
cc -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/libwavpack.a -lm -o .task-build/baseline
.task-build/baseline
