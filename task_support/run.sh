#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/layouts.cc; fi
mkdir -p .task-build
rm -f .task-build/*.o
for src in *.cpp; do
  c++ -std=c++11 -O0 -I. -Itask_support/vendor -c "$src" -o ".task-build/$(basename "${src%.cpp}").o"
done
c++ -std=c++11 -O0 -I. -Itask_support/vendor task_support/baseline.cc .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O0 -I. -Itask_support/vendor tests/layouts.cc .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layouts references legacy; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
