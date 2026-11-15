#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
[[ ${1:-baseline} == baseline ]] || { echo 'usage: bash task_support/run.sh baseline' >&2; exit 2; }
mkdir -p .task-build
${CC:-cc} -std=c99 -O1 -Isrc $(find src -type f -name '*.c' | LC_ALL=C sort) task_support/baseline.c -lm -o .task-build/baseline
.task-build/baseline
