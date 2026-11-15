#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/LodSurfaceTarget.cpp; fi
mkdir -p .task-build
c++ -std=c++14 -O2 -Iinclude -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/PolyVox/LodSurface.h
test -s include/PolyVox/LodSurface.inl
test -s examples/LodSurfaceHeadless.cpp
c++ -std=c++14 -O2 -Iinclude -I. -include PolyVox/LodSurface.h -x c++ task_support/driver.c tests/LodSurfaceTarget.cpp -o .task-build/target
c++ -std=c++14 -O2 -Iinclude -I. -include PolyVox/LodSurface.h examples/LodSurfaceHeadless.cpp -o .task-build/lod-example
.task-build/lod-example
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in seams edits reuse; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
