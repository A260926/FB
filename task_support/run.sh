#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/bb_map_cases.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -I. task_support/make_sample.cpp -o .task-build/make_sample
c++ -std=c++17 -O0 -I. task_support/baseline.cpp -o .task-build/baseline
.task-build/make_sample
if [[ $mode == target ]]; then
  test -s elfio/elfio_bb_map.hpp
test -s examples/bb_map/bb_map.cpp
c++ -std=c++17 -O0 -I. -include elfio/elfio_bb_map.hpp task_support/driver.c tests/bb_map_cases.cpp -o .task-build/target
c++ -std=c++17 -O0 -I. examples/bb_map/bb_map.cpp -o .task-build/bb_map
.task-build/bb_map .task-build/split.o 2 9
.task-build/bb_map .task-build/linked.elf 2 9
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ranges callsites origins; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline .task-build/split.o
.task-build/baseline .task-build/linked.elf
