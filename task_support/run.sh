#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/surface_guides.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s src/fj_surface_guides.h
test -s src/fj_surface_guides.cc
test -s examples/surface_guides.cpp
c++ -std=c++11 -O1 -pthread -I. -Isrc -include src/fj_surface_guides.h -x c++ task_support/driver.c task_tests/surface_guides.cpp -x none .task-build/fj_surface_guides.o .task-build/libfujiyama.a -o .task-build/target
c++ -std=c++11 -O1 -pthread -I. -Isrc examples/surface_guides.cpp .task-build/fj_surface_guides.o .task-build/libfujiyama.a -o .task-build/example
rm -f .task-build/guides.ppm
.task-build/example
test -s .task-build/guides.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture adaptive reconstruct; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
