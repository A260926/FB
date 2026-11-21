#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/plane_partition.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/core -G "Unix Makefiles" -DOVM_ENABLE_APPLICATIONS=OFF -DOVM_ENABLE_UNITTESTS=OFF -DOVM_ENABLE_EXAMPLES=OFF -DOVM_BUILD_DOCUMENTATION=OFF -DCMAKE_BUILD_TYPE=Release >.task-build/configure.log 2>&1
cmake --build .task-build/core -j2 >.task-build/build.log 2>&1
c++ -std=c++17 -Isrc -I.task-build/core/src task_support/baseline.cpp .task-build/core/Build/lib/libOpenVolumeMesh.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/OpenVolumeMesh/Unstable/Topology/PlanePartition.hh
test -s src/OpenVolumeMesh/Unstable/Topology/PlanePartition.cc
test -s .task-build/core/src/CMakeFiles/OpenVolumeMesh.dir/OpenVolumeMesh/Unstable/Topology/PlanePartition.cc.o
ar -t .task-build/core/Build/lib/libOpenVolumeMesh.a | grep -Fxq PlanePartition.cc.o
test -s examples/plane_partition.cpp
c++ -std=c++17 -Isrc -I.task-build/core/src examples/plane_partition.cpp .task-build/core/Build/lib/libOpenVolumeMesh.a -o .task-build/example
rm -f .task-build/partition_example.ovm
.task-build/example .task-build/partition_example.ovm
test -s .task-build/partition_example.ovm
c++ -std=c++17 -I. -Isrc -I.task-build/core/src -include src/OpenVolumeMesh/Unstable/Topology/PlanePartition.hh task_support/driver.c task_tests/plane_partition.cpp .task-build/core/Build/lib/libOpenVolumeMesh.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in closure geometry corners; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
