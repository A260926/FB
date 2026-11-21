#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/obb_cases.cpp; fi
mkdir -p .task-build
c++ -std=c++20 -O2 -pthread -Isrc task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/bvh/v2/obb.h
test -s src/bvh/v2/c_api/obb.cpp
test -s tests/obb_c_smoke.c
test -s examples/obb_preview.cpp
cc -std=c11 -Isrc -c tests/obb_c_smoke.c -o .task-build/obb_c_smoke.o
production_sources=$(find src/bvh/v2 -name '*.cpp' -type f | sort)
test -n "$production_sources"
rm -rf .task-build/production
mkdir -p .task-build/production
for production_source in $production_sources; do
  production_object=.task-build/production/$(echo "$production_source" | tr / _).o
  c++ -std=c++20 -O2 -pthread -Isrc -I. -include src/bvh/v2/obb.h -c "$production_source" -o "$production_object"
done
production_objects=$(find .task-build/production -name '*.o' -type f | sort)
test -n "$production_objects"
c++ -std=c++20 -O2 -pthread -Isrc -I. $production_objects .task-build/obb_c_smoke.o -o .task-build/obb_c_smoke
.task-build/obb_c_smoke
c++ -std=c++20 -O2 -pthread -Isrc -I. $production_objects examples/obb_preview.cpp -o .task-build/obb_preview
rm -f .task-build/obb.ppm
.task-build/obb_preview .task-build/obb.ppm
test -s .task-build/obb.ppm
c++ -std=c++20 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++20 -O2 -pthread -Isrc -I. $production_objects "$PWD/tests/obb_cases.cpp" .task-build/driver.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in frames traversal maintenance c_api; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
