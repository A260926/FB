#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/structured.c; fi
mkdir -p .task-build
if [[ $mode != target ]]; then
cc -std=gnu11 -O0 -I. -Wno-pointer-sign -DDOC_PATH='"docs"' -DBIN_PATH='"."' -include task_support/shm_compat.h afl-tmin.c afl-struct.c -o .task-build/afl-tmin
fi
cc -std=gnu11 -O0 -I. task_support/benign_target.c -o .task-build/benign-target
if [[ $mode == target ]]; then
  test -s afl-struct.h
test -s afl-struct.c
test -s examples/structured_reduce.c
cc -std=gnu11 -O0 -I. -include afl-struct.h -c afl-struct.c -o .task-build/afl-struct.o
cc -std=gnu11 -O0 -I. -Wno-pointer-sign -DDOC_PATH='"docs"' -DBIN_PATH='"."' -include task_support/shm_compat.h -include afl-struct.h afl-tmin.c .task-build/afl-struct.o -o .task-build/afl-tmin
cc -std=gnu11 -O0 -I. task_support/api_consumer.c .task-build/afl-struct.o -o .task-build/api-consumer
.task-build/api-consumer
cc -std=gnu11 -O0 -I. examples/structured_reduce.c .task-build/afl-struct.o -o .task-build/example
rm -f .task-build/example.txt
.task-build/example > .task-build/example.txt
test -s .task-build/example.txt
grep -qx 'STRUCT_EXAMPLE_OK' .task-build/example.txt
cc -std=gnu11 -O0 -I. task_tests/structured.c task_support/driver.c .task-build/afl-struct.o -o .task-build/target
  output_log=$(mktemp .task-build/target-log.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dependency_closure cache_resume tmin_entry; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
