#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/refraction.cpp; fi
mkdir -p .task-build
production_sources=$(find src -type f -name '*.cpp' ! -name 'rasterizer.cpp' -print)
test -n "$production_sources"
c++ -std=c++20 -O1 -DRASTERIZER_HEADLESS -I. task_support/baseline.cpp $production_sources -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/refraction.h
test -s src/refraction.cpp
test -s examples/refraction.cpp
all_sources=$(find src -type f -name '*.cpp' -print)
test -n "$all_sources"
c++ -std=c++20 -O1 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++20 -O1 -DRASTERIZER_HEADLESS -include cmath -include cstdint -include task_support/compat.h -I. task_tests/refraction.cpp .task-build/driver.o $all_sources -o .task-build/target
c++ -std=c++20 -O1 -DRASTERIZER_HEADLESS -include cmath -include cstdint -include task_support/compat.h -I. examples/refraction.cpp $all_sources -o .task-build/example
rm -f .task-build/refraction.ppm
.task-build/example .task-build/refraction.ppm
test -s .task-build/refraction.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture trace transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
