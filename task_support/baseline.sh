#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/build.sh
b="${TASK_BUILD_DIR:-.build}"
cc -std=c11 -I. -Iinclude -I"$b/include" task_support/baseline.c "$b"/lib/* -lm -o "$b/baseline"
"$b/baseline"
