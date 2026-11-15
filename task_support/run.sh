#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/hlbvh.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -pthread -Wno-unknown-pragmas -Itask_support -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s minpt_hlbvh.h
test -s src/hlbvh.cpp
test -s examples/hlbvh.cpp
c++ -std=c++17 -O1 -pthread -Wno-unknown-pragmas -Itask_support -I. checks/hlbvh.cpp task_support/driver.c src/hlbvh.cpp -o .task-build/target
c++ -std=c++17 -O1 -pthread -Wno-unknown-pragmas -Itask_support -I. examples/hlbvh.cpp src/hlbvh.cpp -o .task-build/example
rm -f .task-build/hlbvh.ppm
.task-build/example .task-build/hlbvh.ppm
test -s .task-build/hlbvh.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in keys parallel refit render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
