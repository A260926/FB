#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/sheet_collapse.cpp; fi
target_enabled=OFF
if [[ $mode == target ]]; then
  test -s src/OpenVolumeMesh/Unstable/Topology/SheetCollapse.hh
  test -s src/OpenVolumeMesh/Unstable/Topology/SheetCollapse.cc
  test -s examples/sheet_collapse.cc
  target_enabled=ON
fi
cmake -S task_support -B .task-build -DTASK_TARGET="$target_enabled" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-w -O0 -g0"
cmake --build .task-build --clean-first --parallel 2
if [[ $mode == target ]]; then
   .task-build/api-consumer
rm -f .task-build/sheet-collapse.ovm
.task-build/example .task-build/sheet-collapse.ovm
test -s .task-build/sheet-collapse.ovm
.task-build/baseline .task-build/sheet-collapse.ovm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sheet geometry corners; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
