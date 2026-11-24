#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash task_support/healthy.sh
rm -f .task-build/original-transfer.log
set +e
src/splint -nof -nolib -tmpdir .task-build/tmp -exportlocal -exportfcn -paramuse -modnomods task_support/fixtures/transfer_good.c > .task-build/original-transfer.log 2>&1
status=$?
set -e
cat .task-build/original-transfer.log
[[ $status != 0 ]]
grep -Ei 'parse error|unrecognized|syntax error' .task-build/original-transfer.log
echo BASELINE_CONDITIONAL_TRANSFER_UNSUPPORTED
