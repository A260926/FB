#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/tangent_pipeline.c; fi
mkdir -p .task-build build build build
cc -std=c99 -O2 -I. task_support/implementation.c task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/tangent_pipeline.c
for index in 16 32; do
  flags=""
  if [[ $index == 32 ]]; then flags="-DPAR_SHAPES_T=uint32_t"; fi
  cc -std=c99 -O2 -I. $flags -c task_support/implementation.c -o .task-build/shapes-$index.o
  cc -std=c99 -O2 -I. $flags task_support/api_consumer.c .task-build/shapes-$index.o -lm -o .task-build/api-$index
  .task-build/api-$index
  cc -std=c99 -O2 -I. $flags examples/tangent_pipeline.c .task-build/shapes-$index.o -lm -o .task-build/example-$index
  rm -f .task-build/tangent-$index.txt
  .task-build/example-$index .task-build/tangent-$index.txt
  test -s .task-build/tangent-$index.txt
  cc -std=c99 -O2 -I. $flags test/tangent_pipeline.c task_support/driver.c .task-build/shapes-$index.o -lm -o .task-build/target-$index
  if [[ $index == 16 ]]; then
    cc -std=c99 -O2 -I. test/test_shapes.c test/console-colors.c -lm -o .task-build/upstream-$index
  else
    cc -std=c99 -O2 -I. $flags test/test_shapes.c test/console-colors.c -lm -o .task-build/upstream-$index
  fi
  .task-build/upstream-$index
  if [[ $index == 32 ]]; then
    set +e
    .task-build/target-$index > .task-build/result-32.log 2>&1
    result_status=$?
    set -e
    cat .task-build/result-32.log
    [[ $result_status == 0 ]] || exit "$result_status"
    for group in generation records transforms invalidation; do
      grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" .task-build/result-32.log
    done
    grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' .task-build/result-32.log
  fi
done
cp .task-build/target-16 .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in generation records transforms invalidation; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
