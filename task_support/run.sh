#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/validation_contract.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -Iinclude task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/cista/validation.h
test -s examples/validation_session.cpp
set --
while IFS= read -r p; do set -- "$@" "$p"; done < <(find include -name '*.cpp' -o -name '*.cc')
c++ -std=c++17 -O0 -I. -Iinclude -include cista/validation.h task_support/api_consumer.cpp "$@" -o .task-build/api_consumer
.task-build/api_consumer
c++ -std=c++17 -O0 -I. -Iinclude -include cista/validation.h task_support/driver.c test/validation_contract.cpp "$@" -o .task-build/target
c++ -std=c++17 -O0 -I. -Iinclude -include cista/validation.h examples/validation_session.cpp "$@" -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in regions sharing transactions; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
