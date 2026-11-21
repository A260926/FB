#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/thb_contracts.cpp; fi
if [[ $mode == target ]]; then
 test -s src/LNLib/include/ThbSurface.h
 test -s src/LNLib/Geometry/Surface/ThbSurface.cpp
 test -s examples/thb_sculpt.cpp
 cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=ON
else
 cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET=OFF
fi
cmake --build .task-build/cmake --target baseline -j 4
if [[ $mode == target ]]; then
  test -s src/LNLib/include/ThbSurface.h
test -s src/LNLib/Geometry/Surface/ThbSurface.cpp
test -s examples/thb_sculpt.cpp
find .task-build/cmake -type f \( -name 'ThbSurface.cpp.o' -o -name 'NurbsSurface.cpp.o' -o -name 'thb_contracts.cpp.o' -o -name 'thb_sculpt.cpp.o' \) -delete
rm -f .task-build/cmake/target .task-build/cmake/thb_example .task-build/target
cmake --build .task-build/cmake --target target thb_example -j 4
cp .task-build/cmake/target .task-build/target
rm -f .task-build/thb.obj
.task-build/cmake/thb_example .task-build/thb.obj
test -s .task-build/thb.obj
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in truncation transfer rendering; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/cmake/baseline
