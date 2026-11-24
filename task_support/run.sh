#!/bin/bash
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then exec bash "$(dirname "$0")/platform.sh" "$@"; fi
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/lock_order_regression.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++17 -O0 -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s relacy/lock_order.hpp
test -s example/lock_order_report.cpp
${CXX:-c++} -std=c++17 -O0 -I. -include relacy/lock_order.hpp task_support/api_consumer.cpp -o .task-build/api_consumer
.task-build/api_consumer
${CXX:-c++} -std=c++17 -O0 -I. -include relacy/lock_order.hpp example/lock_order_report.cpp -o .task-build/demo
demo_log=$(mktemp)
if .task-build/demo >"$demo_log" 2>&1; then cat "$demo_log"; else cat "$demo_log"; rm -f "$demo_log"; exit 1; fi
grep -q '^LOCK_ORDER_DEMO_OK$' "$demo_log"
rm -f "$demo_log"
${CXX:-c++} -std=c++17 -O0 -I. -include relacy/lock_order.hpp task_support/driver.c test/lock_order_regression.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in order lifecycle condvar; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
