#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/tufted_laplacian.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -w -I. -Ieigenlib task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s vcg/complex/algorithms/tufted_laplacian.h
test -s examples/tufted_laplacian.cpp
c++ -std=c++17 -O0 -w -I. -Ieigenlib -include vcg/complex/algorithms/tufted_laplacian.h -x c++ task_support/driver.c tests/tufted_laplacian.cpp -o .task-build/target
c++ -std=c++17 -O0 -w -I. -Ieigenlib -include vcg/complex/algorithms/tufted_laplacian.h examples/tufted_laplacian.cpp -o .task-build/example
rm -f .task-build/example.log
.task-build/example > .task-build/example.log
test -s .task-build/example.log
cat .task-build/example.log
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cover intrinsic operator harmonic; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
