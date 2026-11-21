#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/completion.cpp; fi
mkdir -p .task-build
cxx=${CXX:-c++}
production=(src/*.cpp)
for source in "${production[@]}"; do test -s "$source"; done
if [[ $mode == target ]]; then
  test -s include/fg/Completion.hpp
  "$cxx" -std=c++17 -O0 -g -Iinclude -I. -include fg/Completion.hpp "${production[@]}" task_support/current.cpp -o .task-build/current
else
  "$cxx" -std=c++17 -O0 -g -Iinclude -I. "${production[@]}" task_support/current.cpp -o .task-build/current
fi
if [[ $mode == target ]]; then
  "$cxx" -std=c++17 -O0 -g -Iinclude -I. -include fg/Completion.hpp "${production[@]}" task_support/driver.c tests/completion.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in readiness hazards lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then
  .task-build/current --normal
else
  .task-build/current
fi
