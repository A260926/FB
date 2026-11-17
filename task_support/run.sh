#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/sweep_rmf_task.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s src/qaws/qaws_sweep_rmf.c
for task_precision in 0 1; do
 ${CC:-cc} -std=c11 -O0 -g0 -DQAWS_SCALAR_IS_FLOAT=$task_precision -I. -Isrc/qaws -c src/qaws/qaws_sweep_rmf.c -o .task-build/rmf_$task_precision.o
done
${CC:-cc} -std=c11 -O0 -g0 -DQAWS_SCALAR_IS_FLOAT=0 -I. -Isrc/qaws task_support/driver.c tests/sweep_rmf_task.c .task-build/rmf_0.o .task-build/rmf_1.o .task-build/library/libqaws.a -lm -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in span_transport surface_differentials mode_allocator precision_batch; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline legacy; else .task-build/baseline normal; fi
