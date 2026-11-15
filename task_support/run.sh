#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/multipose_cases.cpp; fi
rm -rf .task-build
mkdir -p .task-build/production
c++ -std=c++17 -O1 -pthread -DXATLAS_C_API=1 -I. -c source/xatlas/xatlas.cpp -o .task-build/xatlas-baseline.o
cc -std=c11 -O1 -I. -c task_support/c_api_baseline.c -o .task-build/c_api_baseline.o
for source_file in source/xatlas/*.cpp examples/*.cpp; do
  [[ -f $source_file ]] || continue
  [[ $source_file == source/xatlas/xatlas.cpp ]] && continue
  object_file=${source_file//\//_}
  c++ -std=c++17 -O1 -pthread -DXATLAS_C_API=1 -I. -c "$source_file" -o ".task-build/production/${object_file%.cpp}.o"
done
c++ -std=c++17 -O1 -pthread -DXATLAS_C_API=1 -I. task_support/baseline.cpp .task-build/xatlas-baseline.o .task-build/c_api_baseline.o $(find .task-build/production -type f -name '*.o' -print) -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s task_tests/multipose_c.c
cc -std=c11 -O1 -I. -c task_tests/multipose_c.c -o .task-build/multipose_c.o
c++ -std=c++17 -O1 -pthread -DXATLAS_C_API=1 -I. task_support/driver.c task_tests/multipose_cases.cpp $(find .task-build/production -type f -name '*.o' -print) .task-build/multipose_c.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in metric mapping packing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
