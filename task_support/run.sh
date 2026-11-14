#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/delta_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -Ivendor/glm tests/bsdf_baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Ivendor/glm -x c++ tests/delta_target.cpp task_support/driver.c -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in interface mixture limits; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline --current; else .task-build/baseline; fi
