#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s fiber.h
  test -s fiber.cpp
  test -s fiber_renderer.h
  test -s fiber_renderer.cpp
  c++ -std=c++17 -I. -x c++ -fsyntax-only fiber.h
  c++ -std=c++17 -I. -x c++ -fsyntax-only fiber.cpp
  c++ -std=c++17 -I. -x c++ -fsyntax-only fiber_renderer.h
  c++ -std=c++17 -I. -x c++ -fsyntax-only fiber_renderer.cpp
fi
if [[ $mode == target ]]; then test -s tests/fiber_cases.cpp; fi
rm -rf .task-build
mkdir -p .task-build
extra_objects=()
for source_file in *.cpp; do
  object_file=".task-build/${source_file%.cpp}.o"
  if [[ $source_file == tinyraytracer.cpp ]]; then
    c++ -std=c++17 -O2 -I. -Dmain=upstream_trace_main -c "$source_file" -o "$object_file"
  else
    c++ -std=c++17 -O2 -I. -c "$source_file" -o "$object_file"
    extra_objects+=("$object_file")
  fi
done
c++ -std=c++17 -O2 -I. task_support/baseline.cpp ${extra_objects[@]+"${extra_objects[@]}"} -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. task_support/driver.c tests/fiber_cases.cpp .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry scattering transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
