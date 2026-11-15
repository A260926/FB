#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/streaming_test.cpp; fi
mkdir -p .task-build
production_sources=(task_support/driver.c)
while IFS= read -r file; do production_sources+=("$file"); done < <(find include -type f -name '*.cpp' | sort)
if [[ $mode == baseline ]]; then c++ -std=c++17 -O2 -I. -Iinclude task_support/baseline.cpp -o .task-build/baseline; fi
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Iinclude -x c++ "${production_sources[@]}" tests/streaming_test.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry frontier lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
