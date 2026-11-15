#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/imprint.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug -DTASK_TARGET=OFF > .task-build-config.log 2>&1
cmake --build .task-build --target sisl_baseline -j 8 > .task-build-compile.log 2>&1
if [[ $mode == target ]]; then
  test -s include/sisl_trace_events.h
test -s src/s1310_events.c
test -s include/sisl_imprint.hpp
test -s src/sisl_imprint.cpp
test -s examples/imprint_obj.cpp
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug -DTASK_TARGET=ON > .task-build-config.log 2>&1
rm -f .task-build/target .task-build/imprint_obj
find .task-build/CMakeFiles -type f \( -path "*/target.dir/*" -o -path "*/imprint_obj.dir/*" \) \( -name "*.o" -o -name "*.obj" \) -delete
cmake --build .task-build --target target imprint_obj -j 8 > .task-build-target.log 2>&1
rm -f .task-build/imprint.obj
.task-build/imprint_obj .task-build/imprint.obj
test -s .task-build/imprint.obj
grep -Eq "^v " .task-build/imprint.obj
grep -Eq "^vn " .task-build/imprint.obj
grep -Eq "^f " .task-build/imprint.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in paired_events cell_complex production; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/sisl_baseline
