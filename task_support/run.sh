#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/materials.c; fi
mkdir -p .task-build
rm -f .task-build/*.o .task-build/target .task-build/baseline
for source_file in src/*.c; do cc -std=c99 -O1 -I. -Iinclude -Isrc -c "$source_file" -o ".task-build/$(basename "$source_file" .c).o"; done
cc -std=c99 -O1 -I. -Iinclude -Isrc task_support/baseline.c .task-build/*.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -Iinclude -Isrc task_tests/materials.c task_support/driver.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture lighting clipping; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
