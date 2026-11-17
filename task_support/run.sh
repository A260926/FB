#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/sphere_connection.cpp; fi
mkdir -p .task-build
production=(src/*/*.cpp)
includes=(-I. -Isrc/math -Isrc/common -Isrc/light -Isrc/camera -Isrc/ray_tracer -Isrc/ppm_writer)
c++ -std=c++17 -O2 -Wno-c++20-extensions "${includes[@]}" "${production[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -Wno-c++20-extensions "${includes[@]}" "${production[@]}" task_tests/sphere_connection.cpp -x c++ task_support/driver.c -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in connections visibility lighting; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
