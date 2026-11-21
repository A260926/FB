#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/conformal_cases.cc; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s include/ConformalMap.h
test -s libsrc/conformal_map.cc
test -s utilsrc/mesh_flatten.cc
test -s examples/conformal_map.cc
c++ -std=c++17 -x c++ -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -Iinclude -I. -include include/ConformalMap.h tests/conformal_cases.cc .task-build/objects/conformal_map.o .task-build/driver.o .task-build/libtrimesh.a -o .task-build/target
c++ -std=c++17 -O1 -Iinclude -I. utilsrc/mesh_flatten.cc .task-build/objects/conformal_map.o .task-build/libtrimesh.a -o .task-build/conformal-cli
c++ -std=c++17 -O1 -Iinclude -I. examples/conformal_map.cc .task-build/objects/conformal_map.o .task-build/libtrimesh.a -o .task-build/example
rm -f .task-build/flat.ply .task-build/cli-flat.ply
.task-build/example .task-build
test -s .task-build/flat.ply
.task-build/conformal-cli task_support/disk.ply .task-build/cli-flat.ply
test -s .task-build/cli-flat.ply
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in metric events pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
