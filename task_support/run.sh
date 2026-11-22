#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/loop_region.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  .task-build/driver/api-probe
rm -f .task-build/loop-region.f32
.task-build/driver/loop-example
test -s .task-build/loop-region.f32
.task-build/driver/output-probe
cp .task-build/driver/task-target .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in native converted seek; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/driver/native-baseline
