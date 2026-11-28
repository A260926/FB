#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/edit.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. mikktspace.c task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s mikktspace_edit.h
test -s mikktspace_edit.c
test -s examples/edit.c
cc -std=c99 -O2 -I. -DMIKK_EDIT_TRACE=mikk_edit_trace_face -include task_support/trace.h -c mikktspace.c -o .task-build/mikk.o
cc -std=c99 -O2 -I. -DMIKK_EDIT_TRACE=mikk_edit_trace_face -include task_support/trace.h -include mikktspace_edit.h -c mikktspace_edit.c -o .task-build/edit.o
cc -std=c99 -O2 -I. -c task_support/trace.c -o .task-build/trace.o
cc -std=c99 -O2 -I. task_support/api_consumer.c .task-build/mikk.o .task-build/edit.o .task-build/trace.o -lm -o .task-build/api-consumer
.task-build/api-consumer
cc -std=c99 -O2 -I. examples/edit.c .task-build/mikk.o .task-build/edit.o .task-build/trace.o -lm -o .task-build/example
rm -f .task-build/edit.txt
.task-build/example .task-build/edit.txt
test -s .task-build/edit.txt
cc -std=c99 -O2 -I. test/edit.c task_support/driver.c .task-build/mikk.o .task-build/edit.o .task-build/trace.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in equivalence dependencies locality transaction; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
