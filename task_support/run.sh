#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/splice.c; fi
mkdir -p .task-build
cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release >.task-build/configure.log 2>&1 || { tail -n 24 .task-build/configure.log; exit 2; }
cmake --build .task-build/cmake --target check_baseline check_old_encoders -j 4 >.task-build/build.log 2>&1 || { tail -n 24 .task-build/build.log; exit 3; }
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -O2 -I. -Iinclude -Isrc/libFLAC/include -I.task-build/cmake/flac task_support/driver.c tests/splice.c .task-build/cmake/flac/src/libFLAC/libFLAC.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pcm payload metadata; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/cmake/check_baseline
.task-build/cmake/check_old_encoders
echo BASELINE_OK
