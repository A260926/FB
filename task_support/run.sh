#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_slot_tail.cpp; fi
export ALSOFT_CONF="$PWD/task_support/alsoft.conf"
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s core/effectslottail.cpp
test -s core/effectslottail.h
test -s examples/slot_tail_render.cpp
cmake --build .task-build/build --target task_target task_example -j 4
cp .task-build/build/task_target .task-build/target
rm -f .task-build/slot_tail_render.pcm
.task-build/build/task_example .task-build/slot_tail_render.pcm
test -s .task-build/slot_tail_render.pcm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in handoff clock lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/build/task_baseline
