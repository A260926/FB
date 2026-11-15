#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/beam.cpp; fi
mkdir -p .task-build
CXX=${CXX:-c++}
includes=(-I. -Itask_support)
while IFS= read -r directory; do includes+=("-I$directory"); done < <(find src -type d | LC_ALL=C sort)
production_sources=(src/camera/camera.cpp)
while IFS= read -r source; do
  if [[ "$source" != src/camera/camera.cpp ]]; then production_sources+=("$source"); fi
done < <(find src -mindepth 2 -type f -name '*.cpp' | LC_ALL=C sort)
"$CXX" -std=c++17 -O1 -Wno-c++20-extensions "${includes[@]}" "${production_sources[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/query/beam_traversal.hpp
test -s src/query/beam_traversal.cpp
test -s examples/beam.cpp
"$CXX" -std=c++17 -O1 -Wno-c++20-extensions "${includes[@]}" "${production_sources[@]}" examples/beam.cpp -o .task-build/beam
rm -f .task-build/beam.ppm
.task-build/beam .task-build/beam.ppm
test -s .task-build/beam.ppm
"$CXX" -std=c++17 -O1 -Wno-c++20-extensions "${includes[@]}" -x c++ task_support/driver.c task_tests/beam.cpp "${production_sources[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in frusta traversal updates render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
