#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/quantized_target.cpp; fi
mkdir -p .task-build
export CLANG_MODULE_CACHE_PATH="$PWD/.task-build/clang-cache"
${CXX:-c++} -std=c++14 -O2 -Iinclude task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/FastBVH/QuantizedBVH.h
test -s include/FastBVH/QuantizedTraverser.h
${CXX:-c++} -std=c++14 -O2 -I. -Iinclude -include FastBVH/QuantizedBVH.h -include FastBVH/QuantizedTraverser.h task_support/driver.c tests/quantized_target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in encoding traversal integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
