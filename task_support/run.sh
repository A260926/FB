#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/encoder/EncUT_FrameBudget_task.cpp; fi
mkdir -p .task-build
required_object=
if [[ $mode == target ]]; then
  test -s codec/encoder/core/src/frame_budget.cpp
  test -s codec/encoder/core/inc/frame_budget.h
  c++ -std=c++11 -I. -Icodec/common/inc -Icodec/api -Icodec/encoder/core/inc -x c++ -c -include codec/encoder/core/inc/frame_budget.h /dev/null -o .task-build/required-header.o
fi
make -j4 USE_ASM=No libopenh264.a >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
if [[ $mode == target ]]; then
  test -s codec/encoder/core/src/frame_budget.o
  ar t libopenh264.a | grep -qx frame_budget.o
  required_object="codec/encoder/core/src/frame_budget.o .task-build/required-header.o"
fi
c++ -std=c++11 -I. task_support/baseline.cpp $required_object libopenh264.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/frame_budget_demo.cpp
c++ -std=c++11 -I. -Icodec/api -Icodec/common/inc -Icodec/encoder/core/inc -Icodec/encoder/plus/inc examples/frame_budget_demo.cpp $required_object libopenh264.a -pthread -o .task-build/frame-budget-demo
.task-build/frame-budget-demo
c++ -std=c++11 -I. -Icodec/api -Icodec/common/inc -Icodec/encoder/core/inc -Icodec/encoder/plus/inc task_support/driver.c test/encoder/EncUT_FrameBudget_task.cpp $required_object libopenh264.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in budget rollback reference; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
