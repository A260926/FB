#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/iso_split_contracts.cpp; fi
if [[ $mode == target ]]; then
 test -s opennurbs_brep_isosplit.h
 test -s opennurbs_brep_isosplit.cpp
 printf '#include "opennurbs_brep_isosplit.h"\n' | c++ -std=c++17 -I. -I. -x c++ -fsyntax-only -
fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --target task_baseline -j 8
if [[ $mode == target ]]; then
  cmake --build .task-build --target task_target task_example -j 8
rm -f .task-build/iso_split.3dm
.task-build/task_example .task-build/iso_split.3dm
test -s .task-build/iso_split.3dm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in loops mates persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_baseline
