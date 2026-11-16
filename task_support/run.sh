#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/symmetry.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  set -- .task-build/production-*.o
  if [[ ! -f $1 ]]; then set --; fi
  c++ -std=c++11 -O2 -I. task_tests/symmetry.cpp .task-build/driver.o "$@" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in orbit objective sequence compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
  bash task_support/cli_symmetry.sh
fi
.task-build/baseline
