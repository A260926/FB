#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/operator_lod.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -DEIGEN_MPL2_ONLY -Iinclude -I.deps/eigen task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/igl/qslim_operator.h
test -s include/igl/qslim_operator.cpp
c++ -std=c++17 -O0 -DEIGEN_MPL2_ONLY -I. -Iinclude -include igl/qslim_operator.h -I.deps/eigen task_support/driver.c task_tests/operator_lod.cpp -o .task-build/target
test -s examples/operator_lod.cpp
c++ -std=c++17 -O0 -DEIGEN_MPL2_ONLY -Iinclude -I.deps/eigen examples/operator_lod.cpp -o .task-build/example
rm -f .task-build/coarse.obj .task-build/fine_field.csv
.task-build/example .task-build
test -s .task-build/coarse.obj
test -s .task-build/fine_field.csv
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in schur queue transfer compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
