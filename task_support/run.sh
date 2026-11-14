#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/visibility.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++11 -O2 -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s analytic_visibility.h
  test -s analytic_visibility.cpp
  ${CXX:-c++} -std=c++11 -O2 -I. -x c++ task_support/driver.c task_tests/visibility.cpp analytic_visibility.cpp -o .task-build/target
  test -s examples/area_visibility.cpp
  ${CXX:-c++} -std=c++11 -O2 -I. examples/area_visibility.cpp analytic_visibility.cpp -o .task-build/demo
  .task-build/demo .task-build/area.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection union render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O2 task_support/check_ppm.cpp -o .task-build/check_ppm
  .task-build/check_ppm .task-build/area.ppm
fi
