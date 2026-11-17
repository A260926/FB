#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/selection_contract.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s coffi/coffi_selection.hpp
c++ -std=c++17 -O0 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O0 -I. tests/selection_contract.cpp .task-build/driver.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in selection references ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
