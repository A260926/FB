#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/surface_error.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -O2 -I. -DMESHOPT_EXAMPLE_NO_MAIN -c examples/surface_lod.cpp -o .task-build/surface_lod.o
  c++ -std=c++11 -O2 -I. task_tests/surface_error.cpp .task-build/surface_lod.o .task-build/driver.o .task-build/cmake/libmeshoptimizer.a -o .task-build/target
  output_log=.task-build/target.log
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in continuous invalidation budget compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
