#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/deep_transmission.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -Irender/tests math/*.c render/*.c render/tests/memory_backend.c render/tests/golden.c render/tests/test_render.c -DPINGO_REFERENCE_DIR=\"$PWD/render/tests/references\" -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s render/deep.h
test -s render/deep.c
cc -std=c99 -O2 -I. -Irender/tests math/*.c render/*.c render/tests/memory_backend.c tests/deep_transmission.c task_support/driver.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layers transport lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline empty
.task-build/baseline triangle
.task-build/baseline cube
.task-build/baseline no-uv
