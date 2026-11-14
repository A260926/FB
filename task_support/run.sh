#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_conforming.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -w -I. -c task_support/library.cpp -o .task-build/library.o
c++ -std=c++11 -w -I. task_support/baseline.cpp .task-build/library.o -o .task-build/baseline
c++ -std=c++11 -w demo/tests.cpp -o .task-build/upstream-tests
if [[ $mode == target ]]; then
  c++ -std=c++11 -w -I. -include stdint.h -include stddef.h -x c++ task_support/driver.c tests/task_conforming.cpp -x none .task-build/library.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coverage seams ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
 .task-build/baseline
(cd demo/vox && ../../.task-build/upstream-tests)
