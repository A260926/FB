#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/incremental_subdivision.cpp; fi
mkdir -p .task-build
cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=OFF > .task-build/configure.log 2>&1
cmake --build .task-build/cmake --target task_baseline -j4 > .task-build/build.log 2>&1
if [[ $mode == target ]]; then
  test -s examples/incremental_subdivision.cpp
cmake -S task_support -B .task-build/cmake -DTASK_TARGET=ON > .task-build/configure-target.log 2>&1
cmake --build .task-build/cmake --target task_target task_example -j4 > .task-build/build-target.log 2>&1
cp .task-build/cmake/task_target .task-build/target
rm -f .task-build/current.obj
.task-build/cmake/task_example .task-build/current.obj
test -s .task-build/current.obj
grep -Eq '^v ' .task-build/current.obj
grep -Eq '^f ' .task-build/current.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in arrangement derived locality; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/cmake/task_baseline "$mode"
