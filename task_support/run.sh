#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/SurfaceLightsTarget.cpp; fi
bash task_support/build_cpu.sh
if [[ $mode == target ]]; then
  test -s src/VoxelSurfaceLights.hpp
test -s src/VoxelSurfaceLights.cpp
test -s src/VoxelDirect.cpp
test -s examples/SurfaceLightsHeadless.cpp
c++ -std=c++14 -O2 -Isrc -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++14 -O2 -Isrc -I. -c tests/SurfaceLightsTarget.cpp -o .task-build/cases.o
c++ -pthread .task-build/driver.o .task-build/cases.o @.task-build/objects.rsp -o .task-build/target
c++ -std=c++14 -O2 -pthread -Isrc -I. examples/SurfaceLightsHeadless.cpp @.task-build/objects.rsp -o .task-build/lights-example
.task-build/lights-example
  output_log=$(mktemp 2>/dev/null || echo .task-build/target-run.log)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in exposure distribution transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
