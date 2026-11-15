#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/coarse_shading.cpp; fi
mkdir -p .task-build
production=$(find src -maxdepth 1 -name '*.cpp' ! -name main.cpp ! -name Game.cpp ! -name Window.cpp ! -name Bitmap.cpp ! -name BufferDC.cpp ! -name FileDialog.cpp ! -name Font.cpp ! -name Menu.cpp ! -name Tests.cpp ! -name OBJLoader.cpp ! -name lodepng.cpp -print)
test -n "$production"
c++ -std=c++17 -O1 -Isrc -I. $production task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/CoarseShading.h
test -s src/CoarseShading.cpp
test -s examples/coarse_material.cpp
c++ -std=c++17 -O1 -Isrc -I. -include src/CoarseShading.h $production task_support/driver.c tests/coarse_shading.cpp -o .task-build/target
c++ -std=c++17 -O1 -Isrc -I. -include src/CoarseShading.h $production examples/coarse_material.cpp -o .task-build/coarse_material
rm -f .task-build/coarse_material.ppm
.task-build/coarse_material .task-build/coarse_material.ppm
test -s .task-build/coarse_material.ppm
test "$(head -c 2 .task-build/coarse_material.ppm)" = P6
test "$(wc -c < .task-build/coarse_material.ppm)" -gt 20
  output_log=$(mktemp "${TMPDIR:-/tmp}/output_log.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in grouping interpolation visibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
