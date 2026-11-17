#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/visibility.cpp; fi
mkdir -p .task-build
production_sources=()
for source in src/*.cpp; do
  if [[ "$source" != src/main.cpp ]]; then production_sources+=("$source"); fi
done
c++ -std=c++17 -O2 -pthread -I. -Iinclude "${production_sources[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -pthread -I. -Iinclude "${production_sources[@]}" task_tests/visibility.cpp -x c++ task_support/driver.c -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in query refresh transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
