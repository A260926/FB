#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/packet-seek-contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=gnu11 -O1 -I. -I.task-build/include -Iinclude -Ivendor/ogg/include task_support/driver.c src/tests/packet-seek-contract.c .task-build/liboggz-task.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ordinal resume interleave reject; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline --observe-gap; else .task-build/baseline; fi
