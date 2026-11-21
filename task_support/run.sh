#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/beams.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  output_dir=$(mktemp -d)
.task-build/taskexample "$output_dir"
test -s "$output_dir/beams.ppm"
test "$(wc -c < "$output_dir/beams.ppm")" -gt 64
head -c 2 "$output_dir/beams.ppm" | grep -E "^P[36]$"
rm -rf "$output_dir"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in integral index transport progressive; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/taskbaseline
