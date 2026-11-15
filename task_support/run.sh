#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/beam_target.cpp; fi
mkdir -p .task-build
c++ -std=c++20 -O1 -I. -Isrc/library src/library/base.cpp src/library/cubiquity.cpp src/library/extraction.cpp src/library/geometry.cpp src/library/raytracing.cpp src/library/storage.cpp src/library/utility.cpp task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/library/beam_volume.h
test -s src/library/beam_volume.cpp
test -s src/library/beam_render.h
test -s src/library/beam_render.cpp
test -s examples/beam_preview.cpp
rm -f .task-build/beam.ppm .task-build/beam-example .task-build/target
c++ -std=c++20 -O1 -I. -Isrc/library src/library/base.cpp src/library/cubiquity.cpp src/library/extraction.cpp src/library/geometry.cpp src/library/raytracing.cpp src/library/storage.cpp src/library/utility.cpp src/library/beam_volume.cpp src/library/beam_render.cpp examples/beam_preview.cpp -o .task-build/beam-example
.task-build/beam-example
test -s .task-build/beam.ppm
c++ -std=c++20 -O1 -I. -Isrc/library src/library/base.cpp src/library/cubiquity.cpp src/library/extraction.cpp src/library/geometry.cpp src/library/raytracing.cpp src/library/storage.cpp src/library/utility.cpp src/library/beam_volume.cpp src/library/beam_render.cpp task_support/driver.c tests/beam_target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in correlation clipping sharing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
