#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/rate_checks.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release > .task-build/configure.log 2>&1
cmake --build .task-build/lib --parallel 2 > .task-build/build.log 2>&1
c++ -std=c++11 -Isrc -Itask_support task_support/baseline.cpp .task-build/lib/libmodplug.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -Isrc -I. task_support/api_consumer.cpp .task-build/lib/libmodplug.a -lm -o .task-build/api_consumer
.task-build/api_consumer
c++ -std=c++11 -Isrc -I. tests/rate_checks.cpp task_support/driver.c .task-build/lib/libmodplug.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock tails compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
