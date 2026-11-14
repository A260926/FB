#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/clr_methods_cases.c; fi
bash task_support/build_core.sh
cc -std=c11 task_support/make_sample.c -o .task-build/make_sample
cc -std=c11 -I. -Ilib/libpe/include task_support/baseline.c .task-build/pe_core.o -o .task-build/baseline
.task-build/make_sample
if [[ $mode == target ]]; then
  test -s lib/libpe/clr_methods.c
test -s lib/libpe/include/libpe/clr_methods.h
test -s examples/clr_methods.c
cc -std=c11 -I. -Ilib/libpe/include task_support/driver.c tests/clr_methods_cases.c lib/libpe/clr_*.c .task-build/pe_core.o -o .task-build/target
cc -std=c11 -I. -Ilib/libpe/include examples/clr_methods.c lib/libpe/clr_*.c .task-build/pe_core.o -o .task-build/clr_methods
.task-build/clr_methods .task-build/plain.exe 20485
.task-build/clr_methods .task-build/ptr.exe 20485
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ownership bodies integrity; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline .task-build/plain.exe
.task-build/baseline .task-build/ptr.exe
