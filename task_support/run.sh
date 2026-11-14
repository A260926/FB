#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/spring_chain.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -I. -Isrc -include algorithm -include cstring -x c++ task_support/driver.c test/spring_chain.cpp -x none .task-build/libcal3d.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dynamics contacts pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/pipeline-baseline
.task-build/compression-baseline
