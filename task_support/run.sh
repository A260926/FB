#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/streamclip_target.cpp; fi
if [[ $mode == target ]]; then test -s src/cal3d/streamclip.h; test -s src/cal3d/streamclip.cpp; test -s examples/streamclip.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -I. -Isrc -include algorithm -include cstring -include src/cal3d/streamclip.h task_support/driver.c test/streamclip_target.cpp .task-build/streamclip.o .task-build/libcal3d.a -o .task-build/target
c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -I. -Isrc -include algorithm -include cstring examples/streamclip.cpp .task-build/streamclip.o .task-build/libcal3d.a -o .task-build/example
.task-build/example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in block_sampling cache_transactions mixed_pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
