#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/match_cases.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s libudis86/cfg_normalize.c
test -s libudis86/cfg_match.c
cc -std=c99 -O1 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 task_support/driver.c tests/match_cases.c libudis86/cfg_normalize.c libudis86/cfg_match.c .task-build/libudis.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology aliases schedule; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/libcheck
.task-build/baseline
