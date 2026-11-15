#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/emission_fit.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s examples/fit_emission.cpp
c++ -std=c++17 -O0 -pthread -Iinclude -I. examples/fit_emission.cpp $(cat .task-build/objects.list) -o .task-build/fit-example
rm -f .task-build/fit-example.ppm
.task-build/fit-example .task-build/fit-example.ppm
test -s .task-build/fit-example.ppm
c++ -std=c++17 -O0 -pthread -Iinclude -I. task_support/driver.c task_tests/emission_fit.cpp $(cat .task-build/objects.list) -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry inverse render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
