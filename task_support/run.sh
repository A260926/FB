#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/inset_roof.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -w -Isrc -Ivendor/eigen -c task_support/core.cpp -o .task-build/core.o
c++ -std=c++17 -O0 -w -include iostream -Isrc -Ivendor/eigen task_support/baseline.cpp .task-build/core.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/OpenGP/SurfaceMesh/InsetRoof.h
test -s src/OpenGP/SurfaceMesh/InsetRoof.cpp
test -s examples/inset_roof.cpp
c++ -std=c++17 -O0 -w -include iostream -include src/OpenGP/SurfaceMesh/InsetRoof.h -Isrc -Ivendor/eigen -c src/OpenGP/SurfaceMesh/InsetRoof.cpp -o .task-build/inset-roof.o
c++ -std=c++17 -O0 -w -include iostream -Isrc -Ivendor/eigen -I. task_tests/inset_roof.cpp task_support/driver.c .task-build/inset-roof.o .task-build/core.o -o .task-build/target
c++ -std=c++17 -O0 -w -include iostream -Isrc -Ivendor/eigen task_support/api_consumer.cpp .task-build/inset-roof.o .task-build/core.o -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++17 -O0 -w -DOPENGP_HEADERONLY -include iostream -Isrc -Ivendor/eigen task_support/api_consumer.cpp -o .task-build/header-consumer
.task-build/header-consumer
c++ -std=c++17 -O0 -w -include iostream -Isrc -Ivendor/eigen examples/inset_roof.cpp .task-build/inset-roof.o .task-build/core.o -o .task-build/example
rm -f .task-build/inset-roof.off
.task-build/example .task-build/inset-roof.off
test -s .task-build/inset-roof.off
.task-build/baseline .task-build/inset-roof.off
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in wavefront topology attributes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
