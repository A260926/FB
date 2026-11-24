#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/task_conform.c; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug
cmake --build .task-build --clean-first -j 4
if [[ $mode == target ]]; then
  test -s src/framework/mlt_conform.h
test -s src/framework/mlt_conform.c
test -s examples/conform_render.c
.task-build/binding
rm -f .task-build/conform-example.wav
.task-build/conform_render .task-build/conform-example.wav
test -s .task-build/conform-example.wav
.task-build/wave_check .task-build/conform-example.wav
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock lazy graph; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
