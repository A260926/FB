#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/realmode_contract.c; fi
mkdir -p .task-build
rm -rf .task-build/obj
mkdir .task-build/obj
for source_file in bddisasm/*.c; do
  cc -std=c11 -O1 -DBDDISASM_HAS_MEMSET -DBDDISASM_HAS_VSNPRINTF -I. -Iinc -Ibddisasm/include -c "$source_file" -o ".task-build/obj/$(basename "$source_file" .c).o" >> .task-build/build.log 2>&1
done
rm -f .task-build/libbddisasm.a
ar rcs .task-build/libbddisasm.a .task-build/obj/*.o
cc -std=c99 -I. -Iinc -Ibddisasm/include task_support/baseline.c .task-build/libbddisasm.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s inc/bdx86_realmode.h
test -s bddisasm/bdx86_realmode.c
test -s examples/realmode.c
cc -std=c99 -I. -Iinc -Ibddisasm/include tests/realmode_contract.c task_support/driver.c .task-build/libbddisasm.a -o .task-build/target
cc -std=c99 -I. -Iinc -Ibddisasm/include examples/realmode.c .task-build/libbddisasm.a -o .task-build/realmode
.task-build/realmode
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in aliases returns recursion; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
