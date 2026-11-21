#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/reseam_contracts.cpp; fi
if [[ $mode == target ]]; then
  test -s opennurbs_brep_reseam.h
  test -s opennurbs_brep_reseam.cpp
  test -s examples/reseam.cpp
  cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=ON
else
  cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=OFF
fi
cmake --build .task-build/cmake --target baseline -j 6
if [[ $mode == target ]]; then
  test -s opennurbs_brep_reseam.h
test -s opennurbs_brep_reseam.cpp
test -s examples/reseam.cpp
find .task-build/cmake -type f \( -name 'opennurbs_brep_reseam.cpp.o' -o -name 'reseam_contracts.cpp.o' -o -name 'reseam.cpp.o' \) -delete
rm -f .task-build/cmake/target .task-build/cmake/reseam_example .task-build/target
cmake --build .task-build/cmake --target target reseam_example -j 6
cp .task-build/cmake/target .task-build/target
rm -f .task-build/reseam.3dm
.task-build/cmake/reseam_example .task-build/reseam.3dm
test -s .task-build/reseam.3dm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in quotient adjacency continuation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/cmake/baseline
