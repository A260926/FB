#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/deep_cases.cpp; fi
mkdir -p .task-build
cc -std=c99 -O2 -Idiscrete_mirage/include -c discrete_mirage/src/discrete_mirage.c -o .task-build/core.o
c++ -std=c++11 -O2 -Idiscrete_mirage/include task_support/baseline.cpp .task-build/core.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s discrete_mirage/src/deep_buffer.c
test -s examples/deep_headless.cpp
cc -std=c99 -O2 -Idiscrete_mirage/include -c discrete_mirage/src/deep_buffer.c -o .task-build/deep.o
c++ -std=c++11 -O2 -I. -Idiscrete_mirage/include -x c++ task_support/driver.c tests/deep_cases.cpp -x none .task-build/core.o .task-build/deep.o -lm -o .task-build/target
c++ -std=c++11 -O2 -I. -Idiscrete_mirage/include examples/deep_headless.cpp .task-build/core.o .task-build/deep.o -lm -o .task-build/deep-example
rm -f .task-build/deep.ppm
.task-build/deep-example .task-build/deep.ppm
test -s .task-build/deep.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layers traversal lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
