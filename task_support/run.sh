#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/time_map_test.cpp; fi
cmake -S . -B .task-build -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DOTIO_PYTHON_INSTALL=OFF -DOTIO_AUTOMATIC_SUBMODULES=OFF -DOTIO_CXX_INSTALL=OFF -DOTIO_INSTALL_COMMANDLINE_TOOLS=OFF -DBUILD_TESTING=OFF -DIMATH_BUILD_TESTS=OFF -DIMATH_INSTALL=OFF -DOTIO_SHARED_LIBS=OFF -DMZ_ZLIB=OFF -DMZ_FETCH_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
if [[ $mode != target ]]; then cmake --build .task-build --target task_baseline -j 4; fi
if [[ $mode == target ]]; then
  test -s src/opentimelineio/timeMappedStack.h
test -s src/opentimelineio/timeMappedStack.cpp
test -s examples/time_mapped_stack.cpp
rm -f .task-build/task_support/CMakeFiles/task_target.dir/time_map_test.cpp.o .task-build/src/opentimelineio/CMakeFiles/opentimelineio.dir/timeMappedStack.cpp.o .task-build/task_support/CMakeFiles/time_map_binding.dir/binding.cpp.o .task-build/task_support/CMakeFiles/time_map_example.dir/__/examples/time_mapped_stack.cpp.o
cmake --build .task-build --target task_target time_map_example time_map_binding -j 4
.task-build/time_map_binding
.task-build/time_map_example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in relation nested editing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/task_support/task_baseline; fi
