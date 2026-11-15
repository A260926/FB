#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/paged_octree.cpp; fi
mkdir -p .task-build
for unit in Octree Octant Options math voxeldisplays; do
  c++ -std=c++17 -O1 -I. -c "$unit.cpp" -o ".task-build/$unit.o"
done
c++ -std=c++17 -O1 -I. task_support/baseline.cpp .task-build/Octree.o .task-build/Octant.o .task-build/Options.o .task-build/math.o .task-build/voxeldisplays.o -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s headers/PagedOctree.h
test -s PagedOctree.cpp
test -s examples/paged_frame.cpp
c++ -std=c++17 -O1 -I. -c PagedOctree.cpp -o .task-build/PagedOctree.o
c++ -std=c++17 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++17 -O1 -I. examples/paged_frame.cpp .task-build/PagedOctree.o .task-build/Octree.o .task-build/Octant.o .task-build/Options.o .task-build/math.o .task-build/voxeldisplays.o -o .task-build/paged_frame
rm -f .task-build/frame.ppm
.task-build/paged_frame .task-build/frame.ppm
test -s .task-build/frame.ppm
c++ -std=c++17 -O1 -I. tests/paged_octree.cpp .task-build/driver.o .task-build/PagedOctree.o .task-build/Octree.o .task-build/Octant.o .task-build/Options.o .task-build/math.o .task-build/voxeldisplays.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry paging lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
