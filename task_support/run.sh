#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
 test -s librtmp/test/interleave-test.c
 test -s librtmp/source/rtmp-chunk-scheduler.c
fi
rm -rf .task-build
mkdir -p .task-build
for f in librtmp/source/*.c; do cc -std=c99 -O0 -I. -Ilibrtmp/include -Ilibflv/include -c "$f" -o ".task-build/$(basename "${f%.c}").o"; done
cc -std=c99 -O0 -I. -Ilibflv/include -c libflv/source/amf0.c -o .task-build/amf0.o
if [[ $mode == target ]]; then
  cc -std=c99 -O0 -I. -Ilibrtmp/include -Ilibflv/include task_support/driver.c librtmp/test/interleave-test.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in interleave headers chunk_size; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then
 cc -std=c99 -O0 -I. -Ilibrtmp/include -Ilibflv/include task_support/baseline.c .task-build/*.o -lm -o .task-build/baseline
 .task-build/baseline
fi
