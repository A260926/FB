#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/deform_tests.cpp; fi
cmake -S . -B .task-build/core -DLUZ_NATIVE_OPTIMIZATIONS=OFF -DLUZ_ENABLE_LTO=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/core --target luz_core -j2
c++ -std=c++20 -O2 -pthread -I. -Iinclude/luz task_support/baseline.cpp .task-build/core/libluz_core.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++20 -O2 -pthread -I. -Iinclude/luz -x c++ task_support/driver.c task_support/deform_tests.cpp -x none .task-build/core/libluz_core.a -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/luz-target.XXXXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in refit instances sampling visibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
