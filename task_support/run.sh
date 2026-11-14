#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/recovery_cases.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/build -DBUILD_TESTING=ON -DBUILD_SHARED_LIBS=OFF -DINSTALL_DOCS=OFF
cmake --build .task-build/build --parallel 2
if [[ $mode == target ]]; then
  cc -std=c11 -Wall -Wextra -I. -Iinclude -I.task-build/build/include task_support/driver.c tests/recovery_cases.c .task-build/build/libogg.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sync recovery streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c11 -Wall -Wextra -Iinclude -I.task-build/build/include support/baseline.c .task-build/build/libogg.a -o .task-build/baseline
.task-build/baseline
.task-build/build/test_bitwise
.task-build/build/test_framing
echo OGG_BASELINE_COMPLETE
