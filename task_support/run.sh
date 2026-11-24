#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/qsbr_runtime_contract.c; fi
mkdir -p .task-build
cc=${CC:-cc}
flags="-std=c11 -O1 -pthread -DMI_DEBUG=0 -DMI_GUARDED=0 -DMI_PROFILE=0 -I. -Iinclude"
"$cc" $flags task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/mimalloc/qsbr-runtime.h
test -s src/qsbr/runtime.c
test -s src/qsbr/allocator.c
test -s example/qsbr_runtime.c
# Recompile every production object and consumer on each entry.
"$cc" $flags -include include/mimalloc/qsbr-runtime.h -c src/qsbr/runtime.c -o .task-build/runtime.o
"$cc" $flags -include include/mimalloc/qsbr-runtime.h -c src/qsbr/allocator.c -o .task-build/allocator.o
"$cc" $flags task_support/api_consumer.c .task-build/runtime.o .task-build/allocator.o -lm -o .task-build/api_consumer
.task-build/api_consumer
rm -f .task-build/example .task-build/example.log
"$cc" $flags example/qsbr_runtime.c .task-build/runtime.o .task-build/allocator.o -lm -o .task-build/example
.task-build/example > .task-build/example.log
cat .task-build/example.log
grep -Fxq QSBR_RUNTIME_EXAMPLE_OK .task-build/example.log
"$cc" $flags task_support/driver.c test/qsbr_runtime_contract.c .task-build/runtime.o .task-build/allocator.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in epochs domains lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
