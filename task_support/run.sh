#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s libsrc/split_sheets.cc
  c++ -std=c++17 -I. -Iinclude -x c++ -fsyntax-only libsrc/split_sheets.cc
fi
if [[ $mode == target ]]; then test -s tests/split_sheets.cc; test -s examples/split_sheets.cc; fi
bash task_support/build_core.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -I. -Iinclude -x c++ task_support/driver.c tests/split_sheets.cc -x none .task-build/libtrimesh.a -o .task-build/target
if [[ -f examples/split_sheets.cc ]]; then c++ -std=c++17 -O0 -Iinclude -I. examples/split_sheets.cc .task-build/libtrimesh.a -o .task-build/example; .task-build/example; fi
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pairing fans pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
