#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/paged_cases.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/upstream -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_OpenMP=TRUE >.task-build/config.log 2>&1 || { tail -30 .task-build/config.log; exit 1; }
cmake --build .task-build/upstream --target scimesh --parallel 2 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
c++ -std=c++17 -O2 -Isrc/core -Isrc/third_party -I.task-build/upstream -I. task_support/baseline.cpp .task-build/upstream/libscimesh.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/core/scimesh/paged_texture.h
test -s src/core/scimesh/paged_renderer.h
test -s src/core/paged_texture.cpp
test -s src/core/paged_renderer.cpp
c++ -std=c++17 -O2 -Isrc/core -Isrc/third_party -I.task-build/upstream -I. src/core/paged_texture.cpp src/core/paged_renderer.cpp task_tests/paged_cases.cpp task_support/driver.c .task-build/upstream/libscimesh.a -o .task-build/target
test -s examples/paged_material.cpp
c++ -std=c++17 -O2 -Isrc/core -Isrc/third_party -I.task-build/upstream -I. src/core/paged_texture.cpp src/core/paged_renderer.cpp examples/paged_material.cpp .task-build/upstream/libscimesh.a -o .task-build/example
(
 set -e
 output_dir=$(mktemp -d)
 trap 'rm -rf "$output_dir"' EXIT
 .task-build/example "$output_dir"
 test -s "$output_dir/paged.ppm"
 test -s "$output_dir/residency.txt"
)
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in residency continuation lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
