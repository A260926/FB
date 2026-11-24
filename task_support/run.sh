#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/structured.cpp; fi
mkdir -p .task-build
if [[ $mode != target ]]; then
c++ -std=c++17 -O0 -I. oss-fuzz/type2.cpp task_support/baseline.cpp -o .task-build/baseline
fi
c++ -std=c++17 -O0 task_support/coverage_worker.cpp -o .task-build/coverage-worker
if [[ $mode == target ]]; then
  test -s oss-fuzz/structured.h
test -s oss-fuzz/structured.cpp
test -s examples/structured_reduce.cpp
c++ -std=c++17 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O0 -I. -include oss-fuzz/structured.h -c oss-fuzz/type2.cpp -o .task-build/type2.o
c++ -std=c++17 -O0 -I. -include oss-fuzz/structured.h -c oss-fuzz/structured.cpp -o .task-build/structured.o
c++ -std=c++17 -O0 -I. task_support/baseline.cpp .task-build/type2.o .task-build/structured.o -o .task-build/baseline
c++ -std=c++17 -O0 -I. task_support/api_consumer.cpp .task-build/type2.o .task-build/structured.o -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++17 -O0 -I. oss-fuzz/translate.cpp .task-build/type2.o .task-build/structured.o -o .task-build/translate
c++ -std=c++17 -O0 -I. examples/structured_reduce.cpp .task-build/type2.o .task-build/structured.o -o .task-build/example
rm -f .task-build/example.cpp
.task-build/example > .task-build/example.cpp
test -s .task-build/example.cpp
c++ -std=c++17 -fsyntax-only .task-build/example.cpp
c++ -std=c++17 -O0 -I. task_tests/structured.cpp .task-build/driver.o .task-build/type2.o .task-build/structured.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in tree_dependencies oracle_cache production_entry; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
