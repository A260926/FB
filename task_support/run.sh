#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/envelope_model.c; fi
bash checks/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Iinclude -Isrc -I.task-build/lib tests/envelope_model.c test-dev/simple_module.c task_support/driver.c .task-build/lib/libxmp.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in interpolation release audio; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash checks/baseline.sh
