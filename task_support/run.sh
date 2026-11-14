#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/differentiable_cases.cpp; fi
mkdir -p .task-build
sources=()
for f in *.cpp; do [[ "$f" == main.cpp ]] || sources+=("$f"); done
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++20 -O1 -I. -x c++ task_support/driver.c tests/differentiable_cases.cpp "${sources[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in forward adjoint lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
