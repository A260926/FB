#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/gnu_extension_contract.c; fi
export CHECK_SANITIZERS="-fsanitize=address,undefined -fno-omit-frame-pointer"
sh task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=gnu99 -O1 -g $CHECK_SANITIZERS -I. -Itask_support -Ilib -I.task-build task_support/driver.c tests/gnu_extension_contract.c .task-build/libtar.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layout limits failure model; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
sh task_support/baseline.sh
