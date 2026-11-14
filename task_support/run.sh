#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/region_player_test.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -DWEBVTT_STATIC=1 -I. -Iinclude task_support/driver.c tests/region_player_test.c .task-build/libwebvtt.a -lm -o .task-build/target
cc -std=c99 -DWEBVTT_STATIC=1 -Iinclude examples/vttregions.c .task-build/libwebvtt.a -lm -o .task-build/vttregions
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in region_stream binding_lifetime rollup_history seek_replay svg_output; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -DWEBVTT_STATIC=1 -Iinclude task_support/baseline.c .task-build/libwebvtt.a -o .task-build/baseline
.task-build/baseline
bash task_support/old-tests.sh
