#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == baseline ]]; then
 node task_support/baseline.cjs
elif [[ $mode == target ]]; then
 test -s tests/bilingual.test.cjs
 output_log=$(mktemp)
 trap 'rm -f "$output_log"' EXIT
 set +e
 node --test --test-reporter=tap tests/bilingual.test.cjs >"$output_log" 2>&1
 run_rc=$?
 set -e
 cat "$output_log"
 [[ $run_rc == 0 ]] || exit "$run_rc"
 for group in optimal_path edit_propagation persistence; do
  grep -Eq "^# TASK_GROUP ${group} checks=[1-9][0-9]* COMPLETE$" "$output_log"
 done
 grep -Eq '^# tests [1-9][0-9]*$' "$output_log"
 grep -Eq '^# pass [1-9][0-9]*$' "$output_log"
 for stat in fail cancelled skipped todo; do
  grep -Eq "^# ${stat} 0$" "$output_log"
 done
else
 echo 'expected baseline or target' >&2
 exit 2
fi
