#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_cases.c; fi
mkdir -p .task-build
task_arch_flags=()
if [[ $(uname -s) == Darwin ]]; then task_arch_flags=(-arch x86_64); fi
clang "${task_arch_flags[@]}" -std=c11 -O1 -w -I. -Isrc task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  clang "${task_arch_flags[@]}" -std=c11 -O1 -w -I. -Isrc task_support/driver.c task_support/target.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fragments resume terminal; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
