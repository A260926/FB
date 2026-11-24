#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/structured_target.cpp; fi
mkdir -p .task-build
cc -std=c99 -O0 -I. -c task_support/upstream.c -o .task-build/upstream.o
cc -std=c99 -O0 -I. task_support/normal.c .task-build/upstream.o -lm -o .task-build/normal
if [[ $mode == target ]]; then
  test -s fuzz/structured/driver.h
for unit in model layout shrink driver; do
 test -s "fuzz/structured/$unit.cpp"
 c++ -std=c++11 -O0 -I. -include fuzz/structured/driver.h -c "fuzz/structured/$unit.cpp" -o ".task-build/$unit.o"
done
c++ -std=c++11 -O0 -I. task_support/consumer.cpp .task-build/model.o .task-build/layout.o .task-build/shrink.o .task-build/driver.o .task-build/upstream.o -lm -o .task-build/structured
rm -rf .task-build/consumer-case .task-build/consumer-reduced .task-build/demo
.task-build/structured generate 7 .task-build/consumer-case
test -s .task-build/consumer-case/case.plan
test -s .task-build/consumer-case/case.gltf
.task-build/normal .task-build/consumer-case/case.gltf
.task-build/structured check .task-build/consumer-case
.task-build/structured reduce .task-build/consumer-case .task-build/consumer-reduced --keep shared-strided --budget 16
test -s .task-build/consumer-reduced/case.plan
test -s .task-build/consumer-reduced/case.gltf
test -s .task-build/consumer-reduced/reduction.log
.task-build/normal .task-build/consumer-reduced/case.gltf
.task-build/structured check .task-build/consumer-reduced
test -s examples/structured_demo.cpp
c++ -std=c++11 -O0 -I. examples/structured_demo.cpp .task-build/model.o .task-build/layout.o .task-build/shrink.o .task-build/driver.o .task-build/upstream.o -lm -o .task-build/demo-app
.task-build/demo-app .task-build/demo > .task-build/demo.log
cat .task-build/demo.log
grep -Fx STRUCTURED_DEMO_OK .task-build/demo.log
test -s .task-build/demo/case.gltf
test -s .task-build/demo/reduced/case.gltf
.task-build/normal .task-build/demo/case.gltf
.task-build/normal .task-build/demo/reduced/case.gltf
c++ -std=c++11 -O0 -I. -c task_support/driver.c -o .task-build/test-driver.o
c++ -std=c++11 -O0 -I. test/structured_target.cpp .task-build/test-driver.o .task-build/model.o .task-build/layout.o .task-build/shrink.o .task-build/driver.o .task-build/upstream.o -lm -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/task-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in graph layout reduce resume process; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/normal
