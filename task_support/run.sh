#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/limiter_contract.c; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  cc -std=c11 -O2 -I. -Isrc -Dmalloc=task_malloc -Dcalloc=task_calloc -Drealloc=task_realloc -Dfree=task_free -c src/cmixer.c -o .task-build/cmixer.o
else
  cc -std=c11 -O2 -I. -Isrc -c src/cmixer.c -o .task-build/cmixer.o
fi
cc -std=c11 -O2 -c task_support/alloc_probe.c -o .task-build/alloc_probe.o
if [[ $mode == target ]]; then
  test -s src/cm_limiter.c
  test -s src/cm_limiter.h
  test -s examples/limiter.c
  cc -std=c11 -O2 -I. -Isrc -include src/cm_limiter.h -c task_support/header_consumer.c -o .task-build/header_consumer.o
  cc -std=c11 -O2 -I. -Isrc -Dmalloc=task_malloc -Dcalloc=task_calloc -Drealloc=task_realloc -Dfree=task_free -c src/cm_limiter.c -o .task-build/limiter.o
  cc -std=c11 -O2 -I. -Isrc task_support/baseline.c .task-build/cmixer.o .task-build/limiter.o .task-build/alloc_probe.o -lm -o .task-build/baseline
else
  cc -std=c11 -O2 -I. -Isrc task_support/baseline.c .task-build/cmixer.o -lm -o .task-build/baseline
fi
if [[ $mode == target ]]; then
  cc -std=c11 -O2 -I. -Isrc tests/limiter_contract.c task_support/driver.c .task-build/cmixer.o .task-build/limiter.o .task-build/header_consumer.o .task-build/alloc_probe.o -lm -o .task-build/target
cc -std=c11 -O2 -I. -Isrc examples/limiter.c .task-build/cmixer.o .task-build/limiter.o .task-build/alloc_probe.o -lm -o .task-build/example
cc -std=c11 -O2 task_support/pcmcheck.c -o .task-build/pcmcheck
rm -f .task-build/limited.pcm
.task-build/example .task-build/limited.pcm >.task-build/example.log
grep -qx LIMITER_DEMO_OK .task-build/example.log
test -s .task-build/limited.pcm
.task-build/pcmcheck .task-build/limited.pcm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in peak clock lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
