#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s switch_recovery_test.mjs
 output_log=$(mktemp)
 trap 'rm -f "$output_log"' EXIT
 set +e
 node task_support/driver.mjs >"$output_log" 2>&1
 status=$?
 set -e
 cat "$output_log"
 [[ $status == 0 ]] || exit "$status"
 for group in aliases fallthrough exits; do
  grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
 done
 grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
elif [[ $mode != baseline ]]; then
 echo "usage: bash task_support/run.sh baseline|target" >&2; exit 2
fi
node task_support/baseline.mjs
