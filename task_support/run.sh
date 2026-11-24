#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/spc_preview_checks.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s gme/Spc_Preview.h
test -s gme/Spc_Preview.cpp
ar t .task-build/cmake/gme/libgme.a | grep -q '^Spc_Preview.cpp.o$'
bash task_support/cxx.sh -std=c++11 -O2 -I. -Igme -Itask_support -include gme/Spc_Preview.h task_support/api_consumer.cpp .task-build/cmake/gme/libgme.a -o .task-build/api-consumer
.task-build/api-consumer
bash task_support/cxx.sh -std=c++11 -O2 -I. -Igme -c tests/spc_preview_checks.cpp -o .task-build/tests.o
bash task_support/cxx.sh -x c++ -std=c++11 -I. -c task_support/driver.c -o .task-build/driver.o
bash task_support/cxx.sh .task-build/tests.o .task-build/driver.o .task-build/cmake/gme/libgme.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ports rollback lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
