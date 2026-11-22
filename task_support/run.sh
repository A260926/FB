#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/inband_fec_target.c; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build .task-build -j 4
.task-build/baseline normal
if [[ $mode == target ]]; then
  test -s examples/repair_to_wav.c
test -s task_support/require_fec_api.c
cc -std=c99 -Werror=implicit-function-declaration -Iinclude -Ivendor/opus/include -Ivendor/ogg/include -I.task-build/ogg/include task_support/require_fec_api.c .task-build/libopusfile_task.a .task-build/opus/libopus.a .task-build/ogg/libogg.a -lm -o .task-build/required-fec-api
.task-build/required-fec-api
cc -std=c99 -I. -Iinclude -Ivendor/opus/include -Ivendor/ogg/include -I.task-build/ogg/include task_support/driver.c tests/inband_fec_target.c .task-build/libopusfile_task.a .task-build/opus/libopus.a .task-build/ogg/libogg.a -lm -o .task-build/target
cc -std=c99 -I. -Iinclude -Ivendor/opus/include -Ivendor/ogg/include -I.task-build/ogg/include examples/repair_to_wav.c .task-build/libopusfile_task.a .task-build/opus/libopus.a .task-build/ogg/libogg.a -lm -o .task-build/repair_to_wav
wave_file=$(mktemp .task-build/repaired-XXXXXX)
cat .task-build/hole.opus | .task-build/repair_to_wav "$wave_file"
test -s "$wave_file"
.task-build/baseline wavcheck "$wave_file"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in granules decode_history consumer; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi

