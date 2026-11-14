#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/eh3_cases.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s pe-parser-library/src/cxx_eh3.cpp
c++ -std=c++14 -O1 -iquote . -Ipe-parser-library/include task_support/driver.c tests/eh3_cases.cpp pe-parser-library/src/cxx_eh3.cpp .task-build/libpe.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in mapping dispatch integrity; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
