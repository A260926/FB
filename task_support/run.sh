#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/line_index_contract.cc; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s dwarf/line_index.hh; test -s dwarf/line_index.cc; "${CXX:-c++}" -std=c++11 -I. -Idwarf -Ielf tests/line_index_contract.cc task_support/driver.c .task-build/libreader.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in operation_ranges source_sites ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"${CXX:-c++}" -std=c++11 -I. -Idwarf -Ielf task_support/baseline.cc .task-build/libreader.a -o .task-build/baseline
.task-build/baseline
