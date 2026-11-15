#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s examples/instance_exclusion.cpp
fi
if [[ $mode == target ]]; then test -s task_tests/exclusion.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -x .task-build/target
mkdir -p .task-build/exclusion-example
rm -f .task-build/exclusion-example/exclude_a.ppm
rm -f .task-build/exclusion-example/exclude_b.ppm
.task-build/task-example .task-build/exclusion-example
test -s .task-build/exclusion-example/exclude_a.ppm
test -s .task-build/exclusion-example/exclude_b.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in routes trie state; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
