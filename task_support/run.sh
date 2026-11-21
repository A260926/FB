#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/offset_job.c; fi
mkdir -p .task-build
make -s -j4 -f task_support/upstream.mk .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/sisl_offset_job.h
test -s src/sisl_offset_job.c
test -s examples/offset_job_obj.c
cc -O1 -std=gnu99 -Iinclude -I. -include include/sisl_offset_job.h -c src/sisl_offset_job.c -o .task-build/offset_job.o
for name in s1360 s1365 s1366; do
  cc -O1 -std=gnu99 -Wno-deprecated-non-prototype -Iinclude -Ds1360=task_reference_s1360 -Ds1365=task_reference_s1365 -Ds1366=task_reference_s1366 -c task_support/reference/$name.c -o .task-build/reference_$name.o
done
for name in s1221 s1227 s1424; do
  cc -O1 -std=gnu99 -Wno-deprecated-non-prototype -Iinclude -D$name=task_real_$name -c src/$name.c -o .task-build/real_$name.o
done
cc -O1 -std=gnu99 -Iinclude -I. -c task_support/eval_trace.c -o .task-build/eval_trace.o
cc -O1 -std=gnu99 -Iinclude -I. task_support/driver.c tests/offset_job.c .task-build/offset_job.o .task-build/reference_s1360.o .task-build/reference_s1365.o .task-build/reference_s1366.o .task-build/eval_trace.o .task-build/real_s1221.o .task-build/real_s1227.o .task-build/real_s1424.o .task-build/libsisl.a -lm -o .task-build/target
cc -O1 -std=gnu99 -Iinclude -I. examples/offset_job_obj.c .task-build/offset_job.o .task-build/libsisl.a -lm -o .task-build/example
rm -f .task-build/offset.obj
.task-build/example .task-build/offset.obj
test -s .task-build/offset.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in scheduling ownership compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
