#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/encoder/EncUT_Intra8x8_task.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -O1 -I. -Icodec/api/wels -Icodec/common/inc -Icodec/encoder/core/inc -Icodec/encoder/plus/inc -Icodec/decoder/core/inc -Icodec/decoder/plus/inc test/encoder/EncUT_Intra8x8_task.cpp task_support/driver.c libopenh264.a -pthread -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in prediction syntax reference; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
