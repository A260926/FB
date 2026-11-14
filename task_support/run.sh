#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s testing/standalone_chunks_test.cc; fi
mkdir -p .task-build
sources=(mkvmuxer/*.cc mkvparser/*.cc)
${CXX:-c++} -std=c++17 -O1 -I. "${sources[@]}" task_support/baseline.cc -o .task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++17 -O1 -I. -x c++ "${sources[@]}" task_support/driver.c testing/standalone_chunks_test.cc -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Split Audio Index Compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
