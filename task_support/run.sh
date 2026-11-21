#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/scene_cases.cpp; fi
mkdir -p .task-build
c++ -std=c++14 -O2 -I. -c task_support/upstream.cpp -o .task-build/upstream.o
c++ -std=c++14 -O2 -I. task_support/baseline.cpp .task-build/upstream.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s voxel_scene.hpp
test -s voxel_scene.cpp
test -s examples/scene_headless.cpp
c++ -std=c++14 -O2 -I. -include voxel_scene.hpp -c voxel_scene.cpp -o .task-build/scene.o
c++ -std=c++14 -O2 -I. -x c++ task_support/driver.c tests/scene_cases.cpp -x none .task-build/upstream.o .task-build/scene.o -lm -o .task-build/target
c++ -std=c++14 -O2 -I. examples/scene_headless.cpp .task-build/upstream.o .task-build/scene.o -lm -o .task-build/scene-example
mkdir -p .task-build/scene-output
rm -f .task-build/scene-output/before.xyz .task-build/scene-output/after.xyz
.task-build/scene-example .task-build/scene-output
test -s .task-build/scene-output/before.xyz
test -s .task-build/scene-output/after.xyz
if cmp -s .task-build/scene-output/before.xyz .task-build/scene-output/after.xyz; then
  echo 'example did not change the point cloud' >&2
  exit 1
fi
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dependencies contributors exports; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
