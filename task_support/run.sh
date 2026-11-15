#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/topology_quantization.cpp; fi
cmake -S task_support -B .task-build -DTASK_BUILD_TARGET=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_Python=TRUE
cmake --build .task-build --target baseline --parallel 4
if [[ $mode == target ]]; then
  test -s examples/topology_quantization.cpp
cmake -S task_support -B .task-build -DTASK_BUILD_TARGET=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_DISABLE_FIND_PACKAGE_Python=TRUE
cmake --build .task-build --target task_target task_example --parallel 4
rm -f .task-build/decoded.obj .task-build/stats.txt
.task-build/example .task-build
test -s .task-build/decoded.obj
test -s .task-build/stats.txt
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in closure mapping stream compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
