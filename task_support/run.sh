#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
case "$mode" in baseline|target) ;; *) exit 2 ;; esac
if [[ $mode == target ]]; then test -s test/constraints_target.cpp; fi
mkdir -p .task-build
cc -std=c99 -O0 -fno-modules -c ufbx.c -o .task-build/ufbx.o
c++ -std=c++17 -O0 -fno-modules -I. task_support/baseline.cpp .task-build/ufbx.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O0 -fno-modules -I. -c test/runner.c -o .task-build/upstream.o
cc .task-build/upstream.o .task-build/ufbx.o -lm -o .task-build/upstream
c++ -std=c++17 -O0 -fno-modules -I. task_support/driver.cpp test/constraints_target.cpp .task-build/ufbx.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dependency blend scene; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
  .task-build/upstream -d data -t maya_constraint_zoo -f 7500 ascii >"$output_log" 2>&1
  cat "$output_log"
  grep -q 'Tests passed: 1/1' "$output_log"
  .task-build/upstream -d data -t blender_279_sausage -f 7400 binary >"$output_log" 2>&1
  cat "$output_log"
  grep -q 'Tests passed: 1/1' "$output_log"
fi
.task-build/baseline
