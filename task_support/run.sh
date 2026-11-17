#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/shared_entry_cases.cpp; fi
mkdir -p .task-build
rm -rf .task-build/cfr
mkdir -p .task-build/cfr
rg --files src task_support/generated task_support/bin task_tests -g '*.java' > .task-build/production-sources.txt
javac -proc:none --release 8 -encoding UTF-8 -d .task-build/cfr @.task-build/production-sources.txt
c++ -std=c++17 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
if [[ $mode == target ]]; then
  c++ -std=c++17 -I. task_tests/shared_entry_cases.cpp .task-build/driver.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in structure effects compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
