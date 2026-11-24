#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/replay_tests.c; fi
mkdir -p .task-build
task_native=$(mktemp -d .task-build/native.XXXXXX)
for task_source in DeepState DeepState_UNIX Log Option Stream; do
  cc -std=gnu11 -DHEADLESS -D__unix=1 -Isrc/include -Isrc/lib -c "src/lib/$task_source.c" -o "$task_native/$task_source.o"
done
ar rcs "$task_native/libdeepstate.a" "$task_native/DeepState.o" "$task_native/DeepState_UNIX.o" "$task_native/Log.o" "$task_native/Option.o" "$task_native/Stream.o"
cc -std=gnu11 -D__unix=1 -Isrc/include task_support/baseline.c "$task_native/libdeepstate.a" -lm -o "$task_native/baseline"
if [[ $mode == target ]]; then
  test -s src/include/deepstate/Replay.h
test -s src/lib/Replay_UNIX.c
test -s examples/replay_batch.c
cc -std=gnu11 -DHEADLESS -D__unix=1 -Isrc/include -Isrc/lib -include deepstate/Replay.h -c src/lib/Replay_UNIX.c -o "$task_native/Replay_UNIX.o"
ar rcs "$task_native/libdeepstate.a" "$task_native/Replay_UNIX.o"
cc -std=gnu11 -D__unix=1 -Isrc/include task_support/api_consumer.c "$task_native/libdeepstate.a" -lm -o "$task_native/api_consumer"
"$task_native/api_consumer" "$task_native/consumer-output"
cc -std=gnu11 -D__unix=1 -Isrc/include examples/replay_batch.c "$task_native/libdeepstate.a" -lm -o "$task_native/example"
"$task_native/example" "$task_native/example-output"
test -s "$task_native/example-output/transcript.txt"
grep -qx 'replay-batch normal=2' "$task_native/example-output/transcript.txt"
cc -std=gnu11 -Dprintf=task_report_printf -Dfprintf=task_report_fprintf -I. -c task_support/driver.c -o "$task_native/driver.o"
cc -std=gnu11 -c task_support/report.c -o "$task_native/report.o"
cc -std=gnu11 -D__unix=1 -Isrc/include -I. -c checks/replay_tests.c -o "$task_native/checks.o"
cc "$task_native/driver.o" "$task_native/report.o" "$task_native/checks.o" "$task_native/libdeepstate.a" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in isolation cancel artifacts; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"$task_native/baseline"
