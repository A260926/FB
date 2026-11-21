#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/dag_raster_target.cpp; fi
mkdir -p .task-build
for unit in base geometry storage utility raytracing extraction; do
  c++ -std=c++20 -O1 -pthread -DCUBIQUITY_USE_POOLSTL -Isrc/library -Isrc/application/external -I. -c "src/library/$unit.cpp" -o ".task-build/$unit.o"
done
ar rcs .task-build/libcore.a .task-build/base.o .task-build/geometry.o .task-build/storage.o .task-build/utility.o .task-build/raytracing.o .task-build/extraction.o
c++ -std=c++20 -O1 -pthread -DCUBIQUITY_USE_POOLSTL -Isrc/library -Isrc/application/external -I. task_support/baseline.cpp .task-build/libcore.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/library/dag_rasterizer.h
test -s src/library/depth_hierarchy.h
test -s src/library/dag_rasterizer.cpp
test -s src/library/depth_hierarchy.cpp
test -s examples/dag_preview.cpp
for unit in dag_rasterizer depth_hierarchy; do
  c++ -std=c++20 -O1 -pthread -DCUBIQUITY_USE_POOLSTL -Isrc/library -Isrc/application/external -I. -include src/library/dag_rasterizer.h -include src/library/depth_hierarchy.h -c "src/library/$unit.cpp" -o ".task-build/$unit.o"
done
c++ -std=c++20 -O1 -pthread -Isrc/library -Isrc/application/external -I. -include src/library/dag_rasterizer.h -include src/library/depth_hierarchy.h examples/dag_preview.cpp .task-build/dag_rasterizer.o .task-build/depth_hierarchy.o .task-build/libcore.a -o .task-build/example
rm -f .task-build/dag.ppm
.task-build/example
test -s .task-build/dag.ppm
c++ -std=c++20 -O1 -pthread -Isrc/library -Isrc/application/external -I. -include src/library/dag_rasterizer.h -include src/library/depth_hierarchy.h task_support/driver.c tests/dag_raster_target.cpp .task-build/dag_rasterizer.o .task-build/depth_hierarchy.o .task-build/libcore.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coverage hierarchy sparse; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
