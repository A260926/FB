#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/shadow_tests.c; fi
rm -rf .task-build
mkdir .task-build
for source in src/*.c; do
  object=.task-build/$(basename "${source%.c}").o
  "${CC:-cc}" -std=c99 -O1 -Iinclude -Isrc -I. -c "$source" -o "$object"
done
ar rcs .task-build/libtinygl.a .task-build/*.o
"${CC:-cc}" -std=c99 -O1 -Iinclude -Isrc -I. checks/baseline.c .task-build/libtinygl.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -O1 -Iinclude -Isrc -I. task_support/driver.c checks/shadow_tests.c .task-build/libtinygl.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture projection filtering compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
