#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/afterglow_cases.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/upstream -DCMAKE_BUILD_TYPE=Release -DBUILD_APP=OFF -DBUILD_TESTS=OFF -DBUILD_HEADLESS=OFF -DBUILD_LIB=OFF -DBUILD_GPU=OFF -DBUILD_VULKAN=OFF -DFETCH_DEPENDENCIES=OFF -DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE >.task-build/config.log 2>&1 || { tail -30 .task-build/config.log; exit 1; }
cmake --build .task-build/upstream --target rn_engine --parallel 2 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
c++ -std=c++23 -O2 -Isrc -Iframework -Ithird_party -I. task_support/baseline.cpp .task-build/upstream/librn_engine.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/TemporalResponse.h
test -s src/TemporalResponse.cpp
test -s src/Afterglow.h
test -s src/Afterglow.cpp
c++ -std=c++23 -O2 -Isrc -Iframework -Ithird_party -I. src/TemporalResponse.cpp src/Afterglow.cpp task_tests/afterglow_cases.cpp task_support/driver.c .task-build/upstream/librn_engine.a -o .task-build/target
test -s examples/afterglow.cpp
c++ -std=c++23 -O2 -Isrc -Iframework -Ithird_party -I. src/TemporalResponse.cpp src/Afterglow.cpp examples/afterglow.cpp .task-build/upstream/librn_engine.a -o .task-build/example
(
 set -e
 output_dir=$(mktemp -d)
 trap 'rm -rf "$output_dir"' EXIT
 .task-build/example "$output_dir"
 test -s "$output_dir/afterglow.csv"
 test -s "$output_dir/steady.ppm"
)
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in response transport film; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
