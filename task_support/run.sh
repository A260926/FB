#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/material_session.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -pthread -include cstring -ICpp/Source -c Cpp/Source/Test.cpp -o .task-build/Test.o
c++ -std=c++17 -O1 -pthread -include cstring -ICpp/Source -c Cpp/Source/Maths.cpp -o .task-build/Maths.o
c++ -std=c++17 -O1 -pthread -ICpp/Source -c Cpp/Source/enkiTS/TaskScheduler.cpp -o .task-build/Scheduler.o
c++ -std=c++17 -O1 -pthread -ICpp/Source -c Cpp/Source/enkiTS/TaskScheduler_c.cpp -o .task-build/SchedulerC.o
rm -f .task-build/production.a
if [[ $mode == target ]]; then
  test -s Cpp/Source/MaterialSession.h
  test -s Cpp/Source/MaterialSession.cpp
  c++ -std=c++17 -O1 -pthread -include cstring -ICpp/Source -c Cpp/Source/MaterialSession.cpp -o .task-build/MaterialSession.o
  ar rcs .task-build/production.a .task-build/Test.o .task-build/Maths.o .task-build/Scheduler.o .task-build/SchedulerC.o .task-build/MaterialSession.o
else
  ar rcs .task-build/production.a .task-build/Test.o .task-build/Maths.o .task-build/Scheduler.o .task-build/SchedulerC.o
fi
c++ -std=c++17 -O1 -pthread -ICpp/Source task_support/baseline.cpp .task-build/production.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s Cpp/Source/MaterialSession.h
test -s Cpp/Source/MaterialSession.cpp
test -s examples/material_session.cpp
c++ -std=c++17 -O1 -pthread -include cstring -I. -ICpp/Source -x c++ task_support/driver.c tests/material_session.cpp -x none .task-build/production.a -o .task-build/target
c++ -std=c++17 -O1 -pthread -include cstring -ICpp/Source examples/material_session.cpp .task-build/production.a -o .task-build/example
rm -f .task-build/session.ppm
.task-build/example .task-build/session.ppm
test -s .task-build/session.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in publication lifetime rendering; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
