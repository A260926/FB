#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s examples/skin_shadow.h
  test -s examples/skin_shadow.cpp
  c++ -std=c++17 -O2 -I. -x c++ -fsyntax-only examples/skin_shadow.h
  c++ -std=c++17 -O2 -I. -x c++ -fsyntax-only examples/skin_shadow.cpp
fi
if [[ $mode == target ]]; then test -s test/skin_shadow_cases.cpp; fi
rm -rf .task-build
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -c task_support/cgltf_runtime.cpp -o .task-build/cgltf_runtime.o
for source_file in examples/*.cpp; do
  [[ -f $source_file ]] || continue
  c++ -std=c++17 -O2 -I. -c "$source_file" -o ".task-build/$(basename "$source_file" .cpp).o"
done
c++ -std=c++17 -O2 -I. task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. task_support/driver.c test/skin_shadow_cases.cpp .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology deformation volume; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
