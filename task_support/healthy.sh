#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f .task-build/healthy.log .task-build/anonymous.log
./sparse -Wcontext task_support/fixtures/healthy.c > .task-build/healthy.log 2>&1
if test -s .task-build/healthy.log; then cat .task-build/healthy.log; exit 31; fi
./sparse -Wcontext task_support/fixtures/anonymous.c > .task-build/anonymous.log 2>&1
grep -F "context imbalance in 'anonymous_unbalanced'" .task-build/anonymous.log
echo HEALTHY_CONTEXT_OK
