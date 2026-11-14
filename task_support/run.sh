#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/bounded_weights_target.cpp; fi
mkdir -p .task-build
cxx=${CXX:-c++}
flags=(-std=c++11 -O1 -DTINYOBJLOADER_USE_DOUBLE -I.)
"$cxx" "${flags[@]}" -c task_support/loader.cpp -o .task-build/loader.o
"$cxx" "${flags[@]}" task_support/baseline.cpp .task-build/loader.o -o .task-build/baseline
"$cxx" -std=c++11 -O1 -I. task_support/legacy_weighted.cpp -o .task-build/legacy
"$cxx" -std=c++11 -O0 -pthread -I. tests/tester.cc experimental/stream/stream_obj_loader.cc -o .task-build/upstream-tests
if [[ $mode == target ]]; then
  target_sources=(task_support/driver.c tests/bounded_weights_target.cpp .task-build/loader.o)
if [[ -f bounded_weights.cpp ]]; then target_sources+=(bounded_weights.cpp); fi
"$cxx" "${flags[@]}" "${target_sources[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optimality seams deformation failures; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
./.task-build/baseline
./.task-build/legacy
(cd tests && ../.task-build/upstream-tests --no-color test_negative_vertex_indices test_face_full_index_format test_face_vertex_normal_only)
