#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/moment_oit.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s moment_oit.h
test -s moment_oit.cpp
test -s examples/moment_oit.cpp
c++ -std=c++17 -O2 -I. -include moment_oit.h task_support/driver.c tests/moment_oit.cpp moment_oit.cpp -o .task-build/target
c++ -std=c++17 -O2 -I. -include moment_oit.h examples/moment_oit.cpp moment_oit.cpp -o .task-build/moment_example
example_output_dir=$(mktemp -d .task-build/moment-example.XXXXXX)
.task-build/moment_example "$example_output_dir"
test -s "$example_output_dir/moments.ppm"
  output_log=$(mktemp .task-build/target-log.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture reconstruct lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
