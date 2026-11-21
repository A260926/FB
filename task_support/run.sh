#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/spectral_transfer.cpp; fi
bash task_support/build.sh "$mode"
bash task_support/link.sh .task-build/baseline task_support/baseline.cpp
if [[ $mode == target ]]; then
  test -s examples/spectral_transfer.cpp
bash task_support/link.sh .task-build/target task_support/driver.c tests/spectral_transfer.cpp
bash task_support/link.sh .task-build/example examples/spectral_transfer.cpp
mkdir -p .task-build/example-output
rm -f .task-build/example-output/spectral.ppm
.task-build/example .task-build/example-output
test -s .task-build/example-output/spectral.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in algebra mixture transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
