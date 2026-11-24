#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/chart_constraints.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -O0 -pthread -I. tests/chart_constraints.cpp -x c++ task_support/driver.c -x none .task-build/xatlas.o -o .task-build/target
c++ -std=c++11 -O0 -pthread -I. task_support/api_consumer.cpp .task-build/xatlas.o -o .task-build/api_consumer
.task-build/api_consumer
cc -std=c99 -Werror=implicit-function-declaration -I. -c task_support/c_consumer.c -o .task-build/c_consumer.o
c++ -pthread .task-build/c_consumer.o .task-build/xatlas.o -o .task-build/c_consumer
.task-build/c_consumer
test -s examples/chart_constraints.cpp
c++ -std=c++11 -O0 -pthread -I. examples/chart_constraints.cpp .task-build/xatlas.o -o .task-build/example
rm -f .task-build/charts.svg
.task-build/example .task-build/charts.svg
test -s .task-build/charts.svg
grep -Eq '<svg([ >])' .task-build/charts.svg
grep -Eq '<(polygon|path)([ >])' .task-build/charts.svg
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in grouping parameterization lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
