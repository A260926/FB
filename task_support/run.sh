#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/density_opacity.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=gnu11 -O1 -I. -Isrc -Iext_src/uthash -include task_support/headless.h -c task_support/driver.c -o .task-build/driver.o
cc -std=gnu11 -O1 -I. -Isrc -Iext_src/uthash -include task_support/headless.h -c tests/density_opacity.c -o .task-build/target.o
c++ .task-build/driver.o .task-build/target.o .task-build/production.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in storage cubic marching; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
