#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/covmerge_tests.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  bash task_support/link.sh
  output_log=$(mktemp "${TMPDIR:-/tmp}/covmerge-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in modules epochs processes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
unset HFUZZ_THREAD_NO
.task-build/legacy
