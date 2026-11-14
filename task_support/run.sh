#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/plc_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s tests/plc_model.c
${CC:-cc} -std=c99 -O1 -fno-fast-math -ffp-contract=off -DFLOATING_POINT -DVAR_ARRAYS -DEXPORT= -I. -Iinclude -Ilibspeex -Itask_support/include/speex task_support/driver.c tests/plc_contract.c tests/plc_model.c .task-build/libspeex.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Excitation Synthesis Recovery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
