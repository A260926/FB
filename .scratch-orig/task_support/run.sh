#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/plc_blocks.c; fi
mkdir -p .task-build
objects=()
for source_file in src/*.c; do
  case "$source_file" in src/toast*.c) continue;; esac
  object_file=".task-build/$(basename "${source_file%.c}").o"
  cc -std=c99 -O2 -DNeedFunctionPrototypes=1 -DSASR -DWAV49 -Iinc -I. -c "$source_file" -o "$object_file"
  objects+=("$object_file")
done
ar rcs .task-build/libgsm.a "${objects[@]}"
cc -std=c99 -O2 -DNeedFunctionPrototypes=1 -DSASR -DWAV49 -Iinc -I. task_support/baseline.c .task-build/libgsm.a -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -DNeedFunctionPrototypes=1 -DSASR -DWAV49 -Iinc -I. task_support/driver.c tests/plc_blocks.c .task-build/libgsm.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Excitation Packing Recovery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
