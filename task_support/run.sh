#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/motion_cases.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++20 -O1 -I. -Idiscrete_mirage/include -x c++ task_support/driver.c tests/motion_cases.cpp -x none .task-build/libdmir.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection paths lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
