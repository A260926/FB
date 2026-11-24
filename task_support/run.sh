#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/stream_cases.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -I. -c task_support/upstream.c -o .task-build/upstream.o
cc -std=c99 -O1 -I. task_support/fixture.c .task-build/upstream.o -lm -o .task-build/fixture

if [[ $mode == target ]]; then
  test -s mp4d_stream.c
test -s mp4d_stream.h
test -s examples/mp4frag.c
cc -std=c99 -O1 -I. -include mp4d_stream.h -c mp4d_stream.c -o .task-build/stream.o
cc -std=c99 -O1 -I. task_support/api_probe.c .task-build/stream.o .task-build/upstream.o -lm -o .task-build/api_probe
cc -std=c99 -O1 -I. examples/mp4frag.c .task-build/stream.o .task-build/upstream.o -lm -o .task-build/mp4frag
cc -std=c99 -O1 -I. -c task_support/stream_cases.c -o .task-build/cases.o
cc -std=c99 -O1 -I. task_support/driver.c .task-build/cases.o .task-build/stream.o .task-build/upstream.o -lm -o .task-build/target
rm -f .task-build/fragment.mp4 .task-build/normal.mp4 .task-build/example.txt
.task-build/fixture .task-build/fragment.mp4 .task-build/normal.mp4
.task-build/api_probe .task-build/fragment.mp4
.task-build/fixture emit | .task-build/mp4frag > .task-build/example.txt
test -s .task-build/example.txt
test "$(wc -l < .task-build/example.txt | tr -d ' ')" = 4
head -1 .task-build/example.txt | grep -Fx '1 90000 0 0 3000 6 33554432 000000026580'

  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in defaults address timeline stream failure; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then
  .task-build/fixture .task-build/fragment.mp4 .task-build/normal.mp4
fi
