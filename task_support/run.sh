#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/view_lod_target.cpp; fi
mkdir -p .task-build
c++ -std=c++14 -O1 -DDC_HEADLESS -DGLM_ENABLE_EXPERIMENTAL -I. -IDualContouringSample -Ivendor/glm DualContouringSample/octree.cpp DualContouringSample/qef.cpp DualContouringSample/svd.cpp DualContouringSample/density.cpp task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s DualContouringSample/view_lod.h
test -s DualContouringSample/view_lod.cpp
test -s examples/view_lod_headless.cpp
rm -f .task-build/lod-near.obj .task-build/lod-far.obj .task-build/lod-example .task-build/target
c++ -std=c++14 -O1 -DDC_HEADLESS -DGLM_ENABLE_EXPERIMENTAL -I. -IDualContouringSample -Ivendor/glm DualContouringSample/octree.cpp DualContouringSample/qef.cpp DualContouringSample/svd.cpp DualContouringSample/density.cpp DualContouringSample/view_lod.cpp examples/view_lod_headless.cpp -o .task-build/lod-example
.task-build/lod-example
test -s .task-build/lod-near.obj
test -s .task-build/lod-far.obj
c++ -std=c++14 -O1 -DDC_HEADLESS -DGLM_ENABLE_EXPERIMENTAL -I. -IDualContouringSample -Ivendor/glm DualContouringSample/octree.cpp DualContouringSample/qef.cpp DualContouringSample/svd.cpp DualContouringSample/density.cpp DualContouringSample/view_lod.cpp task_support/driver.c tests/view_lod_target.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection balance history; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
