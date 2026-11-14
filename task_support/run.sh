#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/clustered_lighting.cpp; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  sources=(src/renderer/*.cpp)
"${CXX:-c++}" -std=c++17 -O2 -I. -Isrc/renderer task_support/driver.c tests/clustered_lighting.cpp "${sources[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture clusters lighting; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash checks/baseline.sh
