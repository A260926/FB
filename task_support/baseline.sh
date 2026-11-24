#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/healthy.sh
rm -f .task-build/original-error-return.log
./sparse -Wcontext task_support/fixtures/error_return.c > .task-build/original-error-return.log 2>&1
if test -s .task-build/original-error-return.log; then cat .task-build/original-error-return.log; exit 32; fi
echo BASELINE_NAMED_RESOURCES_COLLAPSED
