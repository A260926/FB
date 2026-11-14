#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/sharpness.cpp; fi
mkdir -p .task-build
for src in opensubdiv/sdc/*.cpp opensubdiv/vtr/*.cpp opensubdiv/far/*.cpp; do
  obj=.task-build/${src//\//_}.o
  if [[ ! -f $obj || $src -nt $obj ]] || find opensubdiv -name '*.h' -newer "$obj" -print -quit | grep -q .; then
    c++ -std=c++14 -O0 -I. -c "$src" -o "$obj"
  fi
done
rm -f .task-build/libosd.a
ar rcs .task-build/libosd.a .task-build/opensubdiv_*.o
c++ -std=c++14 -O0 -I. task_support/baseline.cpp .task-build/libosd.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++14 -O0 -I. task_support/driver.c task_tests/sharpness.cpp .task-build/libosd.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in rules geometry views; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
