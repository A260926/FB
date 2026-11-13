#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s task_support/target_cases.c
bash task_support/baseline.sh
b="${TASK_BUILD_DIR:-.build}"
cc -std=c11 -I. -Iinclude -I"$b/include" task_support/target_cases.c task_support/driver.c "$b"/lib/* -lm -o "$b/target_cases"
"$b/target_cases"
