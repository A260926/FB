#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/obj-fuzz/state_target.cc; fi
mkdir -p .task-build
c++ -std=c++11 -O0 -pthread -I. -c tiny_obj_loader.cc -o .task-build/upstream.o
c++ -std=c++11 -O0 -pthread -I. task_support/normal.cc .task-build/upstream.o -o .task-build/normal
if [[ $mode == target ]]; then
  test -s tests/obj-fuzz/state_model.h
for unit in state_model state_emit state_reduce state_driver; do
 test -s "tests/obj-fuzz/$unit.cc"
 c++ -std=c++11 -O0 -pthread -I. -include tests/obj-fuzz/state_model.h -c "tests/obj-fuzz/$unit.cc" -o ".task-build/$unit.o"
done
c++ -std=c++11 -O0 -pthread -I. task_support/consumer.cc .task-build/state_model.o .task-build/state_emit.o .task-build/state_reduce.o .task-build/state_driver.o .task-build/upstream.o -o .task-build/obj-state
rm -rf .task-build/consumer-case .task-build/consumer-reduced .task-build/demo
.task-build/obj-state generate 7 .task-build/consumer-case
test -s .task-build/consumer-case/case.trace
test -s .task-build/consumer-case/case.obj
test -s .task-build/consumer-case/case.mtl
.task-build/normal .task-build/consumer-case/case.obj .task-build/consumer-case/case.mtl
.task-build/obj-state check .task-build/consumer-case
.task-build/obj-state reduce .task-build/consumer-case .task-build/consumer-reduced --anchor F0 --budget 16
test -s .task-build/consumer-reduced/case.trace
test -s .task-build/consumer-reduced/case.obj
test -s .task-build/consumer-reduced/case.mtl
test -s .task-build/consumer-reduced/reduction.log
.task-build/normal .task-build/consumer-reduced/case.obj .task-build/consumer-reduced/case.mtl
.task-build/obj-state check .task-build/consumer-reduced
test -s examples/obj_state_demo.cc
c++ -std=c++11 -O0 -pthread -I. examples/obj_state_demo.cc .task-build/state_model.o .task-build/state_emit.o .task-build/state_reduce.o .task-build/state_driver.o .task-build/upstream.o -o .task-build/demo-app
.task-build/demo-app .task-build/demo > .task-build/demo.log
cat .task-build/demo.log
grep -Fx OBJ_STATE_DEMO_OK .task-build/demo.log
test -s .task-build/demo/case.obj
test -s .task-build/demo/case.mtl
test -s .task-build/demo/reduced/case.obj
test -s .task-build/demo/reduced/case.mtl
.task-build/normal .task-build/demo/case.obj .task-build/demo/case.mtl
.task-build/normal .task-build/demo/reduced/case.obj .task-build/demo/reduced/case.mtl
c++ -std=c++11 -O0 -I. -c task_support/driver.c -o .task-build/test-driver.o
c++ -std=c++11 -O0 -pthread -I. tests/obj-fuzz/state_target.cc .task-build/test-driver.o .task-build/state_model.o .task-build/state_emit.o .task-build/state_reduce.o .task-build/state_driver.o .task-build/upstream.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in declaration oracle reduce process; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/normal
