#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_ray_scene.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O1 -Iinclude -Idependencies/glm checks/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/tinynurbs/core/ray_scene.h
test -s src/ray_scene.cpp
test -s examples/ray_scene.cpp
c++ -std=c++17 -O1 -I. -Iinclude -Idependencies/glm -include tinynurbs/core/ray_scene.h -c src/ray_scene.cpp -o .task-build/ray_scene.o
c++ -std=c++17 -O1 -I. -Iinclude -Idependencies/glm -include tinynurbs/core/ray_scene.h -c checks/test_ray_scene.cpp -o .task-build/cases.o
c++ -x c++ -std=c++17 -O1 -I. -c task_support/driver.c -o .task-build/driver.o
c++ .task-build/ray_scene.o .task-build/cases.o .task-build/driver.o -o .task-build/target
c++ -std=c++17 -O1 -I. -Iinclude -Idependencies/glm -include tinynurbs/core/ray_scene.h examples/ray_scene.cpp .task-build/ray_scene.o -o .task-build/example
rm -f .task-build/ray_scene.ppm
.task-build/example .task-build/ray_scene.ppm
test -s .task-build/ray_scene.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in roots updates bounds; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
