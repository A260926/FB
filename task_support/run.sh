#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/interpolation.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -Iinclude -I. clip.c gl.c map.c mat.c pipe.c rast.c task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -Iinclude -I. clip.c gl.c map.c mat.c pipe.c rast.c task_support/driver.c task_tests/interpolation.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in modes clipping bindings; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
