#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/local_annotations_contract.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  find task_tests -name "*.java" | sort > .task-build/task-java.txt
  if test -s .task-build/task-java.txt; then javac --release 21 -proc:none -encoding UTF-8 -cp .task-build/classes -d .task-build/classes @.task-build/task-java.txt; fi
  c++ -std=c++11 -I. task_tests/local_annotations_contract.cpp .task-build/driver.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in lifetimes paths compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
