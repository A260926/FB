#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/stems.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -c task_support/codec.c -o .task-build/codec.o
cc -std=c99 -O2 -I. task_support/baseline.c .task-build/codec.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s dr_mp3_stems.h
test -s dr_mp3_stems.c
test -s examples/stems.c
cc -std=c99 -O2 -I. -include dr_mp3_stems.h -c dr_mp3_stems.c -o .task-build/stems.o
cc -std=c99 -O2 -I. task_support/api_probe.c .task-build/stems.o .task-build/codec.o -lm -o .task-build/api-probe
.task-build/api-probe
cc -std=c99 -O2 -I. examples/stems.c .task-build/stems.o .task-build/codec.o -lm -o .task-build/example
.task-build/example
cc -std=c99 -O2 -I. tests/stems.c task_support/driver.c .task-build/stems.o .task-build/codec.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in split timeline seek; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
