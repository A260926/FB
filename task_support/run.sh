#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/frame_deadline.cpp; fi
mkdir -p .task-build
prod=(code/RtpQueue.cpp code/ScreamTx.cpp code/ScreamV2Tx.cpp code/ScreamV2TxStream.cpp)
for f in code/FrameDeadline*.cpp; do [[ ! -f "$f" ]] || prod+=("$f"); done
c++ -std=c++11 -pthread -I. -Icode "${prod[@]}" task_support/packet_owner.cpp task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -pthread -I. -Icode "${prod[@]}" task_support/packet_owner.cpp -x c++ task_support/driver.c task_tests/frame_deadline.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in frames scheduler compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
