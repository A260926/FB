#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s src/pmp/algorithms/corner_edit.h
  test -s src/pmp/algorithms/corner_edit.cpp
  c++ -std=c++20 -O0 -w -I. -Isrc -Iexternal/eigen-5.0.1 -x c++ -fsyntax-only src/pmp/algorithms/corner_edit.h
  c++ -std=c++20 -O0 -w -I. -Isrc -Iexternal/eigen-5.0.1 -x c++ -fsyntax-only src/pmp/algorithms/corner_edit.cpp
fi
if [[ $mode == target ]]; then test -s tests/corner_edit_test.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++20 -O0 -w -I. -Isrc -Iexternal/eigen-5.0.1 tests/corner_edit_test.cpp task_support/driver.c .task-build/core.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fans split_transfer roundtrip; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
