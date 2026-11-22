#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/pending-write-contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s task_support/require_pending_api.c
  cc -std=gnu11 -Werror=implicit-function-declaration -O1 -I.task-build/include -Iinclude -Ivendor/ogg/include task_support/require_pending_api.c .task-build/liboggz-task.a -lm -o .task-build/required-pending-api
  .task-build/required-pending-api
  cc -std=gnu11 -O1 -I. -I.task-build/include -Iinclude -Ivendor/ogg/include task_support/driver.c src/tests/pending-write-contract.c .task-build/liboggz-task.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in admission pages resume streams; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
