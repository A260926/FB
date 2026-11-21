#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_product.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -Wno-unknown-pragmas -Wno-narrowing -Dmain=smallpt_legacy_main -c smallpt.cpp -o .task-build/legacy.o
c++ -std=c++17 -O0 -I. checks/baseline.cpp .task-build/legacy.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s cubemap_product.hpp
test -s cubemap_product.cpp
test -s environment_trace.hpp
test -s environment_trace.cpp
test -s examples/product_environment.cpp
c++ -std=c++17 -O0 -I. -c cubemap_product.cpp -o .task-build/product.o
c++ -std=c++17 -O0 -I. -c environment_trace.cpp -o .task-build/trace.o
c++ -std=c++17 -O0 -I. -include cubemap_product.hpp -include environment_trace.hpp task_support/driver.c checks/test_product.cpp .task-build/product.o .task-build/trace.o .task-build/legacy.o -o .task-build/target
c++ -std=c++17 -O0 -I. examples/product_environment.cpp .task-build/product.o .task-build/trace.o .task-build/legacy.o -o .task-build/example
rm -f .task-build/product.ppm
.task-build/example .task-build/product.ppm
test -s .task-build/product.ppm
head -c 2 .task-build/product.ppm | grep -Eq '^P[36]$' 
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cells product transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
