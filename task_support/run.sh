#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/task_reference_map.cc; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++20 -I. -Iinclude -I.task-build/wabt/include -x c++ -c task_support/driver.c -o .task-build/driver.o
  ${CXX:-c++} -std=c++20 -I. -Iinclude -I.task-build/wabt/include test/task_reference_map.cc .task-build/driver.o .task-build/wabt/libwabt.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sites symbols consumer; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
