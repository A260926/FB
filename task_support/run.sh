#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_knot_removal.cpp; fi
make -s -j2 -f task_support/Makefile core
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++23 -O0 -ffp-contract=off -I. -Iinclude -Itask_support/vendor/eigen checks/test_knot_removal.cpp .task-build/driver.o .task-build/libnurbs.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in prefix tensor rational_bound; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
