#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s unit_tests/test_playout.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s caption/playout.h
test -s caption/diff608.h
test -s src/playout.c
test -s src/diff608.c
test -s examples/caption_playout.c
"${CC:-cc}" -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -I. -Icaption task_support/driver.c unit_tests/test_playout.c .task-build/playout.o .task-build/diff608.o .task-build/libcaption.a -lm -o .task-build/target
"${CC:-cc}" -std=c99 -D_POSIX_C_SOURCE=200809L -O2 -I. -Icaption examples/caption_playout.c .task-build/playout.o .task-build/diff608.o .task-build/libcaption.a -lm -o .task-build/caption_playout
.task-build/task_fixture make .task-build/example-input.txt
rm -f .task-build/example-output.txt
.task-build/caption_playout .task-build/example-input.txt .task-build/example-output.txt
test -s .task-build/example-output.txt
.task-build/task_fixture check .task-build/example-output.txt
export TASK_CAPTION_PLAYOUT="$PWD/.task-build/caption_playout"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in screens deadlines editing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then .task-build/baseline; fi
