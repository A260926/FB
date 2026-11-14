#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/rd_allocation.c; fi
bash checks/build.sh "$mode"
if [[ $mode == target ]]; then
  cc -std=c99 -DTWOLAME_RD_TESTING -O2 -I. -Ichecks -Ilibtwolame task_support/driver.c tests/rd_allocation.c .task-build/libtwolame.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optimality bitstream compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash checks/baseline.sh && bash checks/legacy.sh
