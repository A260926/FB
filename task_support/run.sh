#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/task_strict_video.c; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug
cmake --build .task-build --clean-first -j 4
if [[ $mode == target ]]; then
  test -s src/framework/mlt_frame.c
test -s src/framework/mlt_consumer.c
test -s src/modules/core/transition_luma.c
cmake -E chdir .task-build ctest --output-on-failure -R '^binding_'
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in image graph export; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; else .task-build/baseline --normal-only; fi
