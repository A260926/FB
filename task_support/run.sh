#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/wav_cue_test.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s doc/examples/wav_cues.c
test -s task_support/require_cue_api.c
cc -std=c99 -Werror=implicit-function-declaration -Isrc/include task_support/require_cue_api.c .task-build/cmake/src/libout123/libout123.a .task-build/cmake/src/libout123/modules/libdefaultmodule.a -lm -o .task-build/required-cue-api
rm -f .task-build/required-cue.wav
.task-build/required-cue-api .task-build/required-cue.wav
cc -std=c99 -I. -Isrc/include src/tests/wav_cue_test.c task_support/driver.c .task-build/cmake/src/libout123/libout123.a .task-build/cmake/src/libout123/modules/libdefaultmodule.a -lm -o .task-build/target
cc -std=c99 -Isrc/include doc/examples/wav_cues.c .task-build/cmake/src/libout123/libout123.a .task-build/cmake/src/libout123/modules/libdefaultmodule.a -lm -o .task-build/wav-cues
cc -std=c99 task_support/example_check.c -o .task-build/example-check
rm -f .task-build/example.wav
.task-build/wav-cues .task-build/example.wav
test -s .task-build/example.wav
.task-build/example-check .task-build/example.wav
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in queue container lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/wave-probe
