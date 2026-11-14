#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s renderer/tests/visibility_target.c; fi
mkdir -p .task-build
export CLANG_MODULE_CACHE_PATH="$PWD/.task-build/clang-cache"
${CC:-cc} -std=c99 -O2 -I. task_support/baseline.c renderer/core/graphics.c renderer/core/maths.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s renderer/core/visibility.h
test -s renderer/core/visibility.c
${CC:-cc} -std=c99 -O2 -I. -include renderer/core/visibility.h task_support/driver.c renderer/tests/visibility_target.c renderer/core/visibility.c renderer/core/graphics.c renderer/core/maths.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture replay compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
