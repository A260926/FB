#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/user_clip.c; fi
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libTinyGL.a
for unit in src/*.c; do cc -std=c99 -O2 -Iinclude -I. -c "$unit" -o ".task-build/$(basename "${unit%.c}").o"; done
ar rcs .task-build/libTinyGL.a .task-build/*.o
cc -std=c99 -O2 -Iinclude -I. task_support/baseline.c .task-build/libTinyGL.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -Iinclude -I. tests/user_clip.c task_support/driver.c .task-build/libTinyGL.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in plane_state geometry render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
