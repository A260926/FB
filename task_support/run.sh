#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s libHh/DiskAtlas.h
  test -s libHh/DiskAtlas.cpp
  c++ -std=c++23 -DNDEBUG -I. -x c++ -fsyntax-only libHh/DiskAtlas.h
  c++ -std=c++23 -DNDEBUG -I. -x c++ -fsyntax-only libHh/DiskAtlas.cpp
fi
if [[ $mode == target ]]; then test -s test/DiskAtlas_task_test.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  bash task_support/link.sh
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cut_graph disk_topology uv_persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
