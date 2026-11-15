#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  test -s soft_raster.h
  test -s soft_raster.cpp
  c++ -std=c++17 -I. -x c++ -fsyntax-only soft_raster.h
  c++ -std=c++17 -I. -x c++ -fsyntax-only soft_raster.cpp
fi
if [[ $mode == target ]]; then test -s tests/soft_target.cpp; fi
mkdir -p .task-build
rm -f .task-build/core-*.o
c++ -std=c++17 -O2 -I. -c support/production.cpp -o .task-build/core-original.o
for unit in ./*.cpp src/*.cpp; do
  test -f "$unit" || continue
  case "$unit" in ./sample_*) continue ;; esac
  c++ -std=c++17 -O2 -I. -c "$unit" -o ".task-build/core-$(basename "${unit%.cpp}").o"
done
c++ -std=c++17 -O2 -I. support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O2 -I. tests/soft_target.cpp .task-build/driver.o .task-build/core-*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in forward visibility_gradient snapshot; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
