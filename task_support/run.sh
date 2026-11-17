#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/task_reference_flow.cc; fi
mkdir -p .task-build
if ! cmake -S task_support -B .task-build/wabt -DCMAKE_BUILD_TYPE=Release >.task-build/configure.log 2>&1; then tail -30 .task-build/configure.log; exit 1; fi
if ! cmake --build .task-build/wabt --target task_baseline -j 2 >.task-build/build.log 2>&1; then tail -30 .task-build/build.log; exit 1; fi
if [[ $mode == target ]]; then
  if ! cmake --build .task-build/wabt --target task_target -j 2 >.task-build/target-build.log 2>&1; then tail -30 .task-build/target-build.log; exit 1; fi
if [[ -f src/tools/wasm-ref-flow.cc ]]; then
  if ! cmake --build .task-build/wabt --target wasm-ref-flow -j 2 >.task-build/consumer-build.log 2>&1; then tail -30 .task-build/consumer-build.log; exit 1; fi
fi
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in contexts locals recursion consumer; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/wabt/task_baseline
