#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/caf_sync_target.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build -j 4
if [[ $mode == target ]]; then
  test -s examples/caf_live_record.cpp
c++ -std=c++11 -I. -Ilibaudiofile task_support/driver.c test/caf_sync_target.cpp .task-build/libaudiofile_task.a -lm -o .task-build/target
c++ -std=c++11 -I. -Ilibaudiofile examples/caf_live_record.cpp .task-build/libaudiofile_task.a -lm -o .task-build/caf_live_record
for bits in 16 24; do
 for channels in 1 2; do
  live_file=$(mktemp .task-build/live-XXXXXX)
  .task-build/caf_live_record produce "$live_file" $bits $channels 17,4079,1,9000,3
  test -s "$live_file"
  .task-build/caf_live_record inspect "$live_file" $bits $channels 13100
 done
done
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in snapshots continuation consumer; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline normal
