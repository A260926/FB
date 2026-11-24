#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
rm -f .task-build/transfer-good.log .task-build/transfer-obligation.log
set +e
src/splint -nof -nolib -tmpdir .task-build/tmp -exportlocal -exportfcn -paramuse -modnomods task_support/fixtures/transfer_good.c > .task-build/transfer-good.log 2>&1
good_status=$?
src/splint -nof -nolib -tmpdir .task-build/tmp -exportlocal -exportfcn -paramuse -modnomods task_support/fixtures/transfer_obligation.c > .task-build/transfer-obligation.log 2>&1
bad_status=$?
set -e
# The healthy contract must parse, and the mismatched implementation must be diagnosed.
[[ $good_status == 0 ]] || { cat .task-build/transfer-good.log; echo CONTRACT_PROBE_GOOD_FAILED; exit 41; }
grep -F 'Finished checking' .task-build/transfer-good.log
if grep -Eiq 'parse error|syntax error|unrecognized|conditional ownership' .task-build/transfer-good.log; then cat .task-build/transfer-good.log; exit 42; fi
[[ $bad_status != 0 ]] || { cat .task-build/transfer-obligation.log; echo CONTRACT_PROBE_OBLIGATION_MISSING; exit 43; }
grep -Fi 'conditional ownership' .task-build/transfer-obligation.log
# The actual function and relation must be present in the diagnostic.
grep -F 'retain_object' .task-build/transfer-obligation.log
grep -F 'src' .task-build/transfer-obligation.log
grep -F 'dst' .task-build/transfer-obligation.log
echo TRANSFER_CONTRACT_PROBE_OK
