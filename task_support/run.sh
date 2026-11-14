#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/panorama.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++11 -O2 -I. -Isrc/renderer task_support/baseline.cpp src/renderer/LineClipper.cpp src/renderer/PolyClipper.cpp src/renderer/VertexProcessor.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/renderer/PanoramaRasterizer.h
  test -s src/renderer/PanoramaRasterizer.cpp
  ${CXX:-c++} -std=c++11 -O2 -I. -Isrc/renderer -x c++ task_support/driver.c task_tests/panorama.cpp src/renderer/PanoramaRasterizer.cpp src/renderer/LineClipper.cpp src/renderer/PolyClipper.cpp src/renderer/VertexProcessor.cpp -o .task-build/target
  test -s examples/panorama.cpp
  ${CXX:-c++} -std=c++11 -O2 -I. -Isrc/renderer examples/panorama.cpp src/renderer/PanoramaRasterizer.cpp src/renderer/LineClipper.cpp src/renderer/PolyClipper.cpp src/renderer/VertexProcessor.cpp -o .task-build/demo
  .task-build/demo .task-build/panorama.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coverage interpolation compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -O2 task_support/check_ppm.cpp -o .task-build/check_ppm
  .task-build/check_ppm .task-build/panorama.ppm
fi
