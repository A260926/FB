#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/dual_output_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -Iinclude -Itask_support -I. task_support/driver.c tests/dual_output_contract.c .task-build/libspeex.a -lm -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/speex-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in DualPCM LossDTX Recursive; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
