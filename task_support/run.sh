#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/stochastic.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/core -DBUILD_TESTING=OFF -DSOFT_RASTERIZER_ENABLE_TBB=OFF -DSOFT_RASTERIZER_ENABLE_GUI=OFF
cmake --build .task-build/core --target soft_rasterizer -j 2
c++ -std=c++17 -O1 -Iinclude -Iexternal/glm -Iexternal/simde -I. task_support/baseline.cpp .task-build/core/libsoft_rasterizer.a .task-build/core/libtinyobjloader.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/render/Stochastic.hpp
test -s src/Stochastic.cpp
test -s examples/stochastic.cpp
ar -t .task-build/core/libsoft_rasterizer.a | grep -q 'Stochastic.cpp'
c++ -std=c++17 -O1 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -I. -Iinclude -Iexternal/glm -Iexternal/simde task_tests/stochastic.cpp .task-build/driver.o .task-build/core/libsoft_rasterizer.a .task-build/core/libtinyobjloader.a -o .task-build/target
c++ -std=c++17 -O1 -I. -Iinclude -Iexternal/glm -Iexternal/simde examples/stochastic.cpp .task-build/core/libsoft_rasterizer.a .task-build/core/libtinyobjloader.a -o .task-build/example
rm -f .task-build/stochastic.ppm
.task-build/example .task-build/stochastic.ppm
test -s .task-build/stochastic.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in identity sampling accumulation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
