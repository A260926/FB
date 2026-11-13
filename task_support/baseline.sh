#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
cc -std=c99 -O2 -Isrc/lib task_support/baseline.c .task-build/libshine.a -lm -o .task-build/baseline
.task-build/baseline
