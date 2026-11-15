#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/interface_cut.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/core -G "Unix Makefiles" -DOVM_ENABLE_APPLICATIONS=OFF -DOVM_ENABLE_UNITTESTS=OFF -DOVM_ENABLE_EXAMPLES=OFF -DOVM_BUILD_DOCUMENTATION=OFF -DCMAKE_BUILD_TYPE=Release >.task-build/configure.log 2>&1
cmake --build .task-build/core -j2 >.task-build/build.log 2>&1
c++ -std=c++17 -Isrc -I.task-build/core/src task_support/baseline.cpp .task-build/core/Build/lib/libOpenVolumeMesh.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/OpenVolumeMesh/Unstable/Topology/InterfaceCut.hh
test -s src/OpenVolumeMesh/Unstable/Topology/InterfaceCut.cc
test -s .task-build/core/src/CMakeFiles/OpenVolumeMesh.dir/OpenVolumeMesh/Unstable/Topology/InterfaceCut.cc.o
test -s examples/interface_cut.cc
c++ -std=c++17 -Isrc -I.task-build/core/src examples/interface_cut.cc .task-build/core/Build/lib/libOpenVolumeMesh.a -o .task-build/example
rm -f .task-build/interface_example.ovm
.task-build/example .task-build/interface_example.ovm
test -s .task-build/interface_example.ovm
c++ -std=c++17 -I. -Isrc -I.task-build/core/src task_support/driver.c task_tests/interface_cut.cpp .task-build/core/Build/lib/libOpenVolumeMesh.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sectors attributes persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
