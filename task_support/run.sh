#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/address_provenance.cpp; fi
bash baseline/build.sh
if [[ $mode == target ]]; then
  "${CXX:-clang++}" -std=c++17 -O0 -g -I. -Isrc -x c++ task_support/driver.c tests/address_provenance.cpp -x none .task-build/libraw_pdb.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in streams forward_spans folded_origins diagnostics; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
