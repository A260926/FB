#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/feature_requirements.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++17 -O0 -I. -Isrc -DVIXL_CODE_BUFFER_MALLOC -x c++ task_support/driver.c test/feature_requirements.cpp -x none .task-build/libanalysis.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Alternatives Accumulation Display; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
