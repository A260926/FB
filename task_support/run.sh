#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/target.c; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  cc -std=c99 -I . -I tinyshader tinyshader/tinyshader_unity.c task_support/driver.c checks/target.c -lm -o .task-build/target
test -s examples/comparison.c
cc -std=c99 -I tinyshader tinyshader/tinyshader_unity.c examples/comparison.c -lm -o .task-build/example
.task-build/example > .task-build/example.log
grep -qx COMPARISON_EXAMPLE_OK .task-build/example.log
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in typing lowering compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash checks/baseline.sh
