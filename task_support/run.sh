#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s include/geometrycentral/surface/surface_anchors.h
  test -s src/surface/surface_anchors.cpp
  c++ -std=c++17 -I. -Iinclude -I.deps/eigen -Ideps/happly -Ideps/nanort/include -Ideps/nanoflann/include -x c++ -fsyntax-only include/geometrycentral/surface/surface_anchors.h
  c++ -std=c++17 -I. -Iinclude -I.deps/eigen -Ideps/happly -Ideps/nanort/include -Ideps/nanoflann/include -x c++ -fsyntax-only src/surface/surface_anchors.cpp
fi
if [[ $mode == target ]]; then test -s task_tests/surface_anchors.cpp; fi
mkdir -p .task-build
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-O0 -g0"
cmake --build .task-build --target geometry-central task_baseline task_artifact_check -j 4
if [[ $mode == target ]]; then
  test -s examples/anchored_lod.cpp
c++ -std=c++17 -O0 -g0 -I. -Iinclude -I.deps/eigen -Ideps/happly -Ideps/nanort/include -Ideps/nanoflann/include task_tests/surface_anchors.cpp -x c++ task_support/driver.c -x none .task-build/gc/src/libgeometry-central.a -pthread -o .task-build/target
c++ -std=c++17 -O0 -g0 -Iinclude -I.deps/eigen -Ideps/happly -Ideps/nanort/include -Ideps/nanoflann/include examples/anchored_lod.cpp .task-build/gc/src/libgeometry-central.a -pthread -o .task-build/anchored_lod
rm -rf .task-build/artifacts
.task-build/anchored_lod .task-build/artifacts
.task-build/task_artifact_check .task-build/artifacts
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection lifecycle scheduling; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  .task-build/task_baseline --witness
else
  .task-build/task_baseline
fi
