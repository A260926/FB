#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/session_feedback_tests.cpp; fi
mkdir -p .task-build
for name in shared_memory_blob_sequence execution_metadata runner_result; do
  c++ -std=c++17 -O1 -I. -Ithird_party/abseil -c "centipede/$name.cc" -o ".task-build/$name.o"
done
c++ -std=c++17 -O1 -I. -Ithird_party/abseil task_support/baseline.cpp .task-build/shared_memory_blob_sequence.o .task-build/execution_metadata.o .task-build/runner_result.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s centipede/session_feedback.h
test -s centipede/session_feedback.cc
test -s examples/session_feedback_replay.cpp
c++ -std=c++17 -O1 -I. -Ithird_party/abseil -include centipede/session_feedback.h -c centipede/session_feedback.cc -o .task-build/session_feedback.o
c++ -std=c++17 -O1 -I. -Ithird_party/abseil task_support/api_probe.cpp .task-build/session_feedback.o .task-build/shared_memory_blob_sequence.o .task-build/execution_metadata.o .task-build/runner_result.o -o .task-build/api_probe
.task-build/api_probe
c++ -std=c++17 -O1 -I. -Ithird_party/abseil examples/session_feedback_replay.cpp .task-build/session_feedback.o .task-build/shared_memory_blob_sequence.o .task-build/execution_metadata.o .task-build/runner_result.o -o .task-build/example
example_dir=$(mktemp -d .task-build/example.XXXXXX)
.task-build/example "$example_dir"
test -s "$example_dir/transcript.txt"
c++ -std=c++17 -O1 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -I. -Ithird_party/abseil checks/session_feedback_tests.cpp .task-build/driver.o .task-build/session_feedback.o .task-build/shared_memory_blob_sequence.o .task-build/execution_metadata.o .task-build/runner_result.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in lifecycle framing features; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
