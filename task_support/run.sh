#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s testing/feedback_target.c; fi
node task_support/amalgamate.mjs
${CC:-cc} -std=c99 -O1 -I.task-build -I. task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c99 -O1 -I.task-build -I. testing/feedback_target.c task_support/driver.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in assembly session replay; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
