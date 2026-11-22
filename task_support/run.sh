#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/seek_coalescing/contract.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_FLAC -DMA_NO_MP3 -I. -c miniaudio.c -o .task-build/production.o
cc -std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_FLAC -DMA_NO_MP3 -I. task_support/baseline.c .task-build/production.o -pthread -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/seek_coalescing.c
test -s task_support/require_coalescing_api.c
cc -std=c99 -Werror=implicit-function-declaration -O1 -DMA_NO_DEVICE_IO -DMA_NO_FLAC -DMA_NO_MP3 -I. task_support/require_coalescing_api.c .task-build/production.o -pthread -lm -o .task-build/required-coalescing-api
.task-build/required-coalescing-api
cc -std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_FLAC -DMA_NO_MP3 -I. tests/seek_coalescing/contract.c task_support/driver.c .task-build/production.o -pthread -lm -o .task-build/target
cc -std=c99 -O1 -DMA_NO_DEVICE_IO -DMA_NO_FLAC -DMA_NO_MP3 -I. examples/seek_coalescing.c .task-build/production.o -pthread -lm -o .task-build/seek-demo
rm -f .task-build/seek-demo.log
.task-build/seek-demo > .task-build/seek-demo.log
cat .task-build/seek-demo.log
grep -qx SEEK_COALESCING_DEMO_OK .task-build/seek-demo.log
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in mailbox publication recovery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
