#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/skinned_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -c MeshDecimationTools.cpp -o .task-build/production.o
c++ -std=c++17 -O2 -I. task_support/baseline.cpp .task-build/production.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s example/skinned.cpp
c++ -std=c++17 -O2 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O2 -I. tests/skinned_target.cpp .task-build/driver.o .task-build/production.o -o .task-build/target
c++ -std=c++17 -O2 -I. example/skinned.cpp .task-build/production.o -o .task-build/example
mkdir -p .task-build/skinned-output
rm -f .task-build/skinned-output/lod0.obj .task-build/skinned-output/lod1.obj
.task-build/example .task-build/skinned-output
test -s .task-build/skinned-output/lod0.obj
test -s .task-build/skinned-output/lod1.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in identity collapse palette; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
