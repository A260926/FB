#!/bin/bash
set -euo pipefail
if [[ $(uname -s) != Linux ]]; then exec bash "$(dirname "$0")/platform.sh" "$@"; fi
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/sync_replay_regression.cc; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  test -s syncreplay.h
  test -s syncreplay.cc
  make -B -j2 libmodel.so CPPFLAGS='-Wall -g -O1 -std=gnu++11 -Iinclude -I. -Ddprintf=task_dprintf' OBJECTS='libthreads.o schedule.o model.o threads.o librace.o action.o nodestack.o clockvector.o main.o snapshot-interface.o cyclegraph.o datarace.o impatomic.o cmodelint.o snapshot.o malloc.o mymemory.o common.o mutex.o promise.o conditionvariable.o context.o scanalysis.o execution.o plugins.o task_compat_printf.o syncreplay.o'
else
  make -B -j2 libmodel.so CPPFLAGS='-Wall -g -O1 -std=gnu++11 -Iinclude -I. -Ddprintf=task_dprintf' OBJECTS='libthreads.o schedule.o model.o threads.o librace.o action.o nodestack.o clockvector.o main.o snapshot-interface.o cyclegraph.o datarace.o impatomic.o cmodelint.o snapshot.o malloc.o mymemory.o common.o mutex.o promise.o conditionvariable.o context.o scanalysis.o execution.o plugins.o task_compat_printf.o'
fi
${CXX:-g++} -std=gnu++11 -Iinclude -I. task_support/baseline.cc -L. -Wl,-rpath,'$ORIGIN/..' -lmodel -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s syncreplay.h
test -s syncreplay.cc
test -s example/sync_replay.cc
${CXX:-g++} -std=gnu++11 -Iinclude -I. -include syncreplay.h task_support/api_consumer.cc -L. -Wl,-rpath,'$ORIGIN/..' -lmodel -o .task-build/api_consumer
api_log=$(mktemp)
if timeout 30 .task-build/api_consumer -v1 >"$api_log" 2>&1; then cat "$api_log"; else cat "$api_log"; exit 1; fi
grep -q '^SYNC_REPLAY_API_OK$' "$api_log"
grep -q '^Number of buggy executions: 0$' "$api_log"
rm -f "$api_log"
${CXX:-g++} -std=gnu++11 -Iinclude -I. -include syncreplay.h example/sync_replay.cc -L. -Wl,-rpath,'$ORIGIN/..' -lmodel -o .task-build/demo
record_dir=$(mktemp -d)
if timeout 30 .task-build/demo -v1 --sync-record "$record_dir/trace.txt" >"$record_dir/record.log" 2>&1; then cat "$record_dir/record.log"; else cat "$record_dir/record.log"; exit 1; fi
test -s "$record_dir/trace.txt"
test "$(head -n 1 "$record_dir/trace.txt")" = CDS-SYNC-1
grep -q '^SYNC_REPLAY_DEMO_OK$' "$record_dir/record.log"
if timeout 30 .task-build/demo -v1 --sync-replay "$record_dir/trace.txt" >"$record_dir/replay.log" 2>&1; then cat "$record_dir/replay.log"; else cat "$record_dir/replay.log"; exit 1; fi
grep -q '^SYNC_REPLAY_DEMO_OK$' "$record_dir/replay.log"
${CXX:-g++} -std=gnu++11 -I. -include syncreplay.h task_support/driver.c test/sync_replay_regression.cc -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in roundtrip choices divergence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
timeout 30 .task-build/baseline -v1
