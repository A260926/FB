#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/extract_scene_target.cpp; fi
rm -rf .task-build
mkdir .task-build
cc -std=c99 -O1 -I. -c ufbx.c -o .task-build/ufbx.o
cc -std=c99 -O1 -I. task_support/baseline.c .task-build/ufbx.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/extract_scene.c
cc -std=c99 -Werror=implicit-function-declaration -I. task_support/extract-consumer.c .task-build/ufbx.o -lm -o .task-build/consumer
.task-build/consumer
cc -std=c99 -Werror=implicit-function-declaration -I. examples/extract_scene.c .task-build/ufbx.o -lm -o .task-build/example
.task-build/example
c++ -std=c++17 -I. test/extract_scene_target.cpp task_support/driver.c .task-build/ufbx.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in closure identity animation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
