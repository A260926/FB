#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/surface_fit.cpp; fi
mkdir -p .task-build
make -s -j4 -f task_support/Makefile .task-build/baseline > .task-build/build.log 2>&1
if [[ $mode == target ]]; then
  test -s src/GEL/HMesh/SurfaceFit.h
test -s src/GEL/HMesh/SurfaceFit.cpp
test -s examples/surface_fit.cpp
make -s -j4 -f task_support/Makefile target > .task-build/build-target.log 2>&1
rm -f .task-build/current.obj
.task-build/example .task-build/current.obj
test -s .task-build/current.obj
grep -Eq '^v ' .task-build/current.obj
grep -Eq '^f ' .task-build/current.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in witness system iteration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
