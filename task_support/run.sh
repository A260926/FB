#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_periodic.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O0 -pthread -I. -c source/xatlas/xatlas.cpp -o .task-build/xatlas.o
c++ -std=c++11 -O0 -pthread -I. checks/baseline.cpp .task-build/xatlas.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s source/xatlas/periodic_constraints.h
test -s source/xatlas/periodic_constraints.cpp
test -s examples/periodic_uv.cpp
c++ -std=c++11 -O0 -pthread -I. -include source/xatlas/periodic_constraints.h -c source/xatlas/periodic_constraints.cpp -o .task-build/periodic.o
c++ -std=c++11 -O0 -pthread -I. task_support/driver.c checks/test_periodic.cpp .task-build/xatlas.o .task-build/periodic.o -o .task-build/target
c++ -std=c++11 -O0 -pthread -I. examples/periodic_uv.cpp .task-build/xatlas.o .task-build/periodic.o -o .task-build/example
rm -f .task-build/periodic.obj
.task-build/example .task-build/periodic.obj
test -s .task-build/periodic.obj
grep -Eq '^vt[[:space:]]' .task-build/periodic.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in mapping constraints solve; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
