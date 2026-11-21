#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/pn_cases.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DCMAKE_PROJECT_INCLUDE="$PWD/task_support/export-source.cmake" -DCMAKE_BUILD_TYPE=Release -DNO_IPO=ON -DCCACHE_PROGRAM:FILEPATH=OFF -DCMAKE_C_COMPILER_LAUNCHER= > .task-build/configure.log 2>&1
cmake --build .task-build/cmake --target lib common -j 2 > .task-build/build.log 2>&1
production_objects=$(find .task-build/cmake/CMakeFiles/lib.dir .task-build/cmake/CMakeFiles/common.dir -name '*.o' -type f | sort)
test -n "$production_objects"
cc -std=c99 -D_POSIX_C_SOURCE=200112L -O2 -Iinclude -Isrc -Isrc/lib -I. task_support/baseline.c $production_objects -pthread -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/lib/datatypes/pn_triangle.h
test -s src/lib/datatypes/pn_triangle.c
test -s examples/pn_preview.c
cc -std=c99 -D_POSIX_C_SOURCE=200112L -O2 -Iinclude -Isrc -Isrc/lib -I. examples/pn_preview.c $production_objects -pthread -lm -o .task-build/pn_preview
rm -f .task-build/pn.ppm
.task-build/pn_preview .task-build/pn.ppm
test -s .task-build/pn.ppm
cc -std=c99 -D_POSIX_C_SOURCE=200112L -O2 -Iinclude -Isrc -Isrc/lib -I. -include src/lib/datatypes/pn_triangle.h task_support/driver.c task_tests/pn_cases.c $production_objects -pthread -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry roots integration protocol; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
