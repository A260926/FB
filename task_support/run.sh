#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/dynamic_cpp.cpp; fi
if [[ $mode == target ]]; then test -s checks/dynamic_c.c; fi
rm -rf .task-build
mkdir .task-build
for source in $(find src -type f -name '*.cpp' | sort); do
  object=.task-build/$(echo "$source" | tr / _).o
  "${CXX:-c++}" -std=c++20 -O1 -pthread -DBVH_BUILD_API -Isrc -I. -c "$source" -o "$object"
done
ar rcs .task-build/libbvh.a .task-build/*.o
"${CXX:-c++}" -std=c++20 -O1 -pthread -Isrc -I. checks/baseline.cpp -o .task-build/baseline_cpp
"${CC:-cc}" -std=c99 -O1 -Isrc -I. -c checks/baseline_c.c -o .task-build/baseline_c.o
"${CXX:-c++}" -pthread .task-build/baseline_c.o .task-build/libbvh.a -o .task-build/baseline_c
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -Isrc -I. -c task_support/driver.c -o .task-build/driver.o
"${CC:-cc}" -std=c99 -DTASK_C_ONLY -Isrc -I. -c task_support/driver.c -o .task-build/driver_c.o
"${CC:-cc}" -std=c99 -O1 -Isrc -I. -c checks/dynamic_c.c -o .task-build/dynamic_c.o
"${CXX:-c++}" -pthread .task-build/driver_c.o .task-build/dynamic_c.o .task-build/libbvh.a -o .task-build/c_target
"${CXX:-c++}" -std=c++20 -O1 -pthread -Isrc -I. checks/dynamic_cpp.cpp .task-build/driver.o .task-build/libbvh.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology identity persistence locality; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then
  c_output=$(mktemp)
  set +e
  .task-build/c_target >"$c_output" 2>&1
  c_status=$?
  set -e
  cat "$c_output"
  if [[ $c_status != 0 ]]; then rm -f "$c_output"; exit "$c_status"; fi
  grep -Eq '^GROUP c_api cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$' "$c_output"
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$c_output"
  rm -f "$c_output"
fi
.task-build/baseline_cpp
.task-build/baseline_c
