#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/retransmit.cpp; fi
if [[ $mode == target ]]; then
 test -s src/rtpretransmit.h
 test -s src/rtpretransmit.cpp
 test -s examples/retransmit.cpp
fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -I.task-build -Isrc -include src/rtpretransmit.h task_support/api_consumer.cpp .task-build/libproduction.a -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++11 -I. -I.task-build -Isrc -include src/rtpretransmit.h task_support/driver.c task_tests/retransmit.cpp .task-build/libproduction.a -o .task-build/target
rm -f .task-build/retransmit-example
c++ -std=c++11 -I.task-build -Isrc -include src/rtpretransmit.h examples/retransmit.cpp .task-build/libproduction.a -o .task-build/retransmit-example
.task-build/retransmit-example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in history feedback session; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
