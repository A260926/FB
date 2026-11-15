#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/scene_nodes.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >.task-configure.log 2>&1
cmake --build .task-build --target task_baseline -j 2 >.task-build.log 2>&1
if [[ $mode == target ]]; then
  find .task-build/CMakeFiles/target.dir -name '*.o' -delete
  cmake --build .task-build --target target -j 2 >>.task-build.log 2>&1
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in hierarchy visibility refresh; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_baseline
if [[ $mode == target ]]; then
  test -s examples/scene_nodes.cpp
  cmake --build .task-build --target scene_nodes_example -j 2 >>.task-build.log 2>&1
  .task-build/scene_nodes_example .task-build/scene-nodes
fi
