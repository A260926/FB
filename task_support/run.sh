#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/reclassify_target.cpp; fi
if [[ $mode == target ]]; then test -s tests/reclassify_c_smoke.c; test -s examples/reclassify_preview.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c11 -I. -c tests/reclassify_c_smoke.c -o .task-build/c-smoke.o
c++ -pthread .task-build/c-smoke.o .task-build/capi.o -o .task-build/c-smoke
.task-build/c-smoke
c++ -std=c++20 -O1 -pthread -DTINYBVH_NO_SIMD -I. examples/reclassify_preview.cpp .task-build/library.o -o .task-build/example
rm -f .task-build/reclassify.ppm
.task-build/example .task-build/reclassify.ppm
test -s .task-build/reclassify.ppm
c++ -std=c++20 -O1 -pthread -DTINYBVH_NO_SIMD -I. task_support/driver.c tests/reclassify_target.cpp .task-build/library.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in split_plan moving_references ray_pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
