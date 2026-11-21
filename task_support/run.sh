#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/light_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -Iinclude -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/PolyVox/LightVolume.h
test -s include/PolyVox/LitCubicSurfaceExtractor.h
test -s examples/lit_voxels.cpp
c++ -std=c++17 -O1 -Iinclude -I. -include PolyVox/LightVolume.h -include PolyVox/LitCubicSurfaceExtractor.h examples/lit_voxels.cpp -o .task-build/example
rm -f .task-build/lit.ply
.task-build/example
test -s .task-build/lit.ply
c++ -std=c++17 -O1 -Iinclude -I. -include PolyVox/LightVolume.h -include PolyVox/LitCubicSurfaceExtractor.h task_support/driver.c tests/light_target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in propagation editing lit_mesh; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
