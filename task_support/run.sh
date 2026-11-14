#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/pose_palette_cache_test.cpp; fi
flag=OFF
[[ $mode != target ]] || flag=ON
cmake -S task_support -B .task-build -DTASK_TARGET="$flag" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_FULLY_DISCONNECTED=ON >.task-config.log 2>&1 || { cat .task-config.log; exit 1; }
cmake --build .task-build --target clean >>.task-build.log 2>&1
cmake --build .task-build --target task_baseline -j 4 >.task-build.log 2>&1 || { tail -n 40 .task-build.log; exit 1; }
if [[ $mode == target ]]; then
  cmake --build .task-build --target task_target -j 4 >>.task-build.log 2>&1 || { tail -n 40 .task-build.log; exit 1; }
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in values invalidation work; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_baseline
