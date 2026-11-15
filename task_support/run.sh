#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/material_program.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++17 -O2 -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  sources=$(find sr -type f -name '*.cpp' -print)
${CXX:-c++} -std=c++17 -O2 -I. task_support/driver.c tests/material_program.cpp $sources -o .task-build/target
test -s examples/material_program.cpp
${CXX:-c++} -std=c++17 -O2 -I. examples/material_program.cpp $sources -o .task-build/example
rm -f .task-build/material.ppm
.task-build/example .task-build/material.ppm
test -s .task-build/material.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in compiler differentials lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
