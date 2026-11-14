#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/nack_retransmit.cpp; fi
rm -rf .task-build
mkdir -p .task-build
for f in src/net/*.cpp src/xop/*.cpp; do c++ -std=c++11 -O0 -pthread -I. -Isrc -Isrc/net -Isrc/xop -Isrc/3rdpart -c "$f" -o ".task-build/$(basename "${f%.cpp}").o"; done
if [[ $mode == target ]]; then
  cc -std=c99 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O0 -pthread -I. -Isrc -Isrc/net -Isrc/xop -Isrc/3rdpart test/nack_retransmit.cpp .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in feedback snapshots transports; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode != target ]]; then
  c++ -std=c++11 -pthread -I. -Isrc -Isrc/net -Isrc/xop -Isrc/3rdpart task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
  .task-build/baseline
fi
