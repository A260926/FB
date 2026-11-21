#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/DielectricTarget.cpp; fi
bash task_support/build_cpu.sh
if [[ $mode == target ]]; then
  test -s src/VoxelInterfaces.hpp
test -s src/VoxelInterfaces.cpp
test -s src/VoxelDielectric.hpp
test -s src/VoxelDielectric.cpp
test -s examples/DielectricHeadless.cpp
c++ -std=c++14 -O2 -Isrc -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++14 -O2 -Isrc -I. -include src/VoxelInterfaces.hpp -include src/VoxelDielectric.hpp -c tests/DielectricTarget.cpp -o .task-build/cases.o
c++ -pthread .task-build/driver.o .task-build/cases.o @.task-build/objects.rsp -o .task-build/target
c++ -std=c++14 -O2 -pthread -Isrc -I. examples/DielectricHeadless.cpp @.task-build/objects.rsp -o .task-build/dielectric-example
rm -f .task-build/dielectric.ppm
.task-build/dielectric-example
test -s .task-build/dielectric.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in interfaces optics transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
