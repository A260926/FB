#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/quadrangulate.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -DOPENGP_HEADERONLY -I. -Isrc -Ivendor/eigen task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/OpenGP/SurfaceMesh/Quadrangulate.h
test -s src/OpenGP/SurfaceMesh/Quadrangulate.cpp
test -s examples/quadrangulate.cpp
c++ -std=c++17 -DOPENGP_HEADERONLY -I. -Isrc -Ivendor/eigen examples/quadrangulate.cpp src/OpenGP/SurfaceMesh/Quadrangulate.cpp -o .task-build/example
rm -f .task-build/quadrangulate_example.obj
.task-build/example .task-build/quadrangulate_example.obj
test -s .task-build/quadrangulate_example.obj
c++ -std=c++17 -DOPENGP_HEADERONLY -I. -Isrc -Ivendor/eigen task_support/driver.c task_tests/quadrangulate.cpp src/OpenGP/SurfaceMesh/Quadrangulate.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in matching topology obj; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
