#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/test_libFLAC/resumable_cases.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  test -s examples/c/decode_resumable.c
cc -std=c99 -O2 -UNDEBUG -DFLAC__NO_DLL -I. -Iinclude -Itask_support task_support/api_probe.c .task-build/cmake/upstream/src/libFLAC/libFLAC.a -lm -o .task-build/api_probe
.task-build/api_probe
cc -std=c99 -O2 -UNDEBUG -DFLAC__NO_DLL -I. -Iinclude -Itask_support examples/c/decode_resumable.c .task-build/cmake/upstream/src/libFLAC/libFLAC.a -lm -o .task-build/decode_resumable
rm -f .task-build/resumable.pcm
.task-build/decode_resumable .task-build/resumable.pcm
test -s .task-build/resumable.pcm
cc -std=c99 -O2 -UNDEBUG -DFLAC__NO_DLL -I. -Iinclude -Itask_support -c task_support/driver.c -o .task-build/driver.o
cc -std=c99 -O2 -UNDEBUG -DFLAC__NO_DLL -I. -Iinclude -Itask_support -c src/test_libFLAC/resumable_cases.c -o .task-build/cases.o
cc .task-build/driver.o .task-build/cases.o .task-build/cmake/upstream/src/libFLAC/libFLAC.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Fragment Entropy Lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 cc -std=c99 -O2 -UNDEBUG -DFLAC__NO_DLL -I. -Iinclude -Itask_support task_support/baseline.c .task-build/cmake/upstream/src/libFLAC/libFLAC.a -lm -o .task-build/baseline
 .task-build/baseline
fi
