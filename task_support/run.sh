#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/decode/linked_groups.cpp; fi
cmake -S . -B .task-build/lib -DARIBCC_NO_RENDERER=ON -DARIBCC_BUILD_TESTS=OFF -DARIBCC_SHARED_LIBRARY=OFF -DCMAKE_BUILD_TYPE=Release >.task-config.log 2>&1
cmake --build .task-build/lib -j2 >.task-build.log 2>&1
cc -I. -c task_support/driver.c -o .task-build/driver.o
if [[ $mode == target ]]; then
  c++ -std=c++17 -include task_support/check_linkage.hpp -I. -Iinclude -I.task-build/lib/include test/decode/linked_groups.cpp .task-build/driver.o .task-build/lib/libaribcaption.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in link_transport publish_barrier lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
 c++ -std=c++17 -Iinclude -I.task-build/lib/include task_support/baseline.cpp .task-build/lib/libaribcaption.a -o .task-build/baseline
 .task-build/baseline
fi
