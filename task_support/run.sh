#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/direction_checks.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s src/core/soloud_core_direction.cpp
c++ -std=c++17 -O2 -Iinclude -Itask_support task_support/api_consumer.cpp .task-build/libsoloud.a -pthread -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++17 -O2 -I. -Iinclude -c tests/direction_checks.cpp -o .task-build/tests.o
c++ -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ .task-build/tests.o .task-build/driver.o .task-build/libsoloud.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in transport fraction storage; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
