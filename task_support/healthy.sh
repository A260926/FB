#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/tmp
rm -f .task-build/healthy.log
src/splint -nof -nolib -tmpdir .task-build/tmp -exportlocal -exportfcn -paramuse -modnomods task_support/fixtures/healthy.c > .task-build/healthy.log 2>&1
cat .task-build/healthy.log
grep -F 'Finished checking' .task-build/healthy.log
echo HEALTHY_OWNERSHIP_OK
