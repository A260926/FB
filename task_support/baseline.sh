#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
cc -O2 -Iinclude task_support/baseline.c .task-build/libopus.a -lm -o .task-build/baseline
.task-build/baseline
