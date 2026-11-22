#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/half_rate.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Itask_support tests/half_rate.c task_support/codec.c task_support/driver.c -lm -o .task-build/target
cc -std=c99 -O2 -DMINIMP3_NO_SIMD -I. -Itask_support tests/half_rate.c task_support/codec.c task_support/driver.c -lm -o .task-build/target-scalar
.task-build/target-scalar
cc -std=c99 -O2 -DMINIMP3_FLOAT_OUTPUT -DMINIMP3_NO_SIMD -I. -Itask_support tests/half_rate.c task_support/codec.c task_support/driver.c -lm -o .task-build/target-float
.task-build/target-float
test -s examples/half_rate.c
cc -std=c99 -O2 -I. examples/half_rate.c task_support/codec.c -lm -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in synthesis timeline compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
