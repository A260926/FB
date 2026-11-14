#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/webvtt_track.c; fi
mkdir -p .task-build/tmp
export TMPDIR="$PWD/.task-build/tmp"
mkdir -p .task-build
cc -std=c99 -O2 -I. -c task_support/implementation.c -o .task-build/minimp4.o
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. task_support/driver.c tests/webvtt_track.c .task-build/minimp4.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline container demux; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
