#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/causal_trace_cases.cpp; fi
mkdir -p .task-build
rm -f .task-build/action.o .task-build/vector_clock.o .task-build/checker.o
for unit in action vector_clock checker; do
  if [[ $mode == target ]]; then
    c++ -std=c++20 -O0 -Iinclude -include model/causal_trace.hpp -c src/model/$unit.cpp -o .task-build/$unit.o
  else
    c++ -std=c++20 -O0 -Iinclude -c src/model/$unit.cpp -o .task-build/$unit.o
  fi
done
c++ -std=c++20 -O0 -Iinclude .task-build/action.o .task-build/vector_clock.o .task-build/checker.o task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/model/causal_trace.hpp
test -s src/model/causal_trace.cpp
test -s examples/causal_trace/main.cpp
c++ -std=c++20 -O0 -Iinclude -include model/causal_trace.hpp .task-build/action.o .task-build/vector_clock.o .task-build/checker.o src/model/causal_trace.cpp task_support/api_consumer.cpp -o .task-build/api-consumer
.task-build/api-consumer
c++ -std=c++20 -O0 -Iinclude -Isrc/cli -include model/causal_trace.hpp .task-build/action.o .task-build/vector_clock.o .task-build/checker.o src/model/causal_trace.cpp src/cli/main.cpp src/cli/program_parser.cpp src/cli/report.cpp -o .task-build/dpor
rm -f .task-build/causal-example .task-build/causal-example.out
c++ -std=c++20 -O0 -Iinclude -include model/causal_trace.hpp .task-build/action.o .task-build/vector_clock.o .task-build/checker.o src/model/causal_trace.cpp examples/causal_trace/main.cpp -o .task-build/causal-example
.task-build/causal-example > .task-build/causal-example.out
test -s .task-build/causal-example.out
cat .task-build/causal-example.out
c++ -std=c++20 -O0 -Iinclude -I. -include model/causal_trace.hpp .task-build/action.o .task-build/vector_clock.o .task-build/checker.o src/model/causal_trace.cpp -x c++ task_support/driver.c tests/causal_trace_cases.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in identity edges cancel cli; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
