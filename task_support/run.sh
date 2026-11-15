#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/transient_pass.cpp; fi
mkdir -p .task-build
production=$(find src -name '*.cpp' ! -name main_render.cpp ! -name main_bench.cpp -print)
test -n "$production"
c++ -std=c++17 -O1 -pthread -Iinclude -I. $production task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/softgpu/render_pass.hpp
test -s examples/transient_material.cpp
c++ -std=c++17 -O1 -pthread -Iinclude -I. -include include/softgpu/render_pass.hpp $production task_support/driver.c tests/transient_pass.cpp -o .task-build/target
c++ -std=c++17 -O1 -pthread -Iinclude -I. -include include/softgpu/render_pass.hpp $production examples/transient_material.cpp -o .task-build/transient_material
rm -f .task-build/transient_material.ppm
.task-build/transient_material .task-build/transient_material.ppm
test -s .task-build/transient_material.ppm
test "$(head -c 2 .task-build/transient_material.ppm)" = P6
test "$(wc -c < .task-build/transient_material.ppm)" -gt 20
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in planning aliasing pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
