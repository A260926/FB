#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/lr_coarsen.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug -DTASK_TARGET=OFF > .task-build-config.log 2>&1
cmake --build .task-build --target lr_baseline -j 8 > .task-build-compile.log 2>&1
if [[ $mode == target ]]; then
  test -s lrsplines2D/src/LRCoarsen.C
test -s examples/lr_coarsen_obj.cpp
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug -DTASK_TARGET=ON > .task-build-config.log 2>&1
rm -f .task-build/target .task-build/lr_coarsen_obj
find .task-build/CMakeFiles -type f \( -path "*/target.dir/*" -o -path "*/lr_coarsen_obj.dir/*" \) \( -name "*.o" -o -name "*.obj" \) -delete
cmake --build .task-build --target target lr_coarsen_obj -j 8 > .task-build-target.log 2>&1
rm -f .task-build/lr-coarsen.obj
.task-build/lr_coarsen_obj .task-build/lr-coarsen.obj
test -s .task-build/lr-coarsen.obj
grep -Eq "^v " .task-build/lr-coarsen.obj
grep -Eq "^vn " .task-build/lr-coarsen.obj
grep -Eq "^f " .task-build/lr-coarsen.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in local_basis homogeneous continuation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/lr_baseline
