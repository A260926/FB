#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/result_state_contract.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  bash task_support/probe.sh
${CC:-cc} -std=c99 -I. task_support/driver.c test/result_state_contract.c -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/result-state-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in declaration flow provenance; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then bash task_support/baseline.sh; else bash task_support/healthy.sh; fi
