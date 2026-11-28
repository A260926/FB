#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/coherent_target.cpp; fi
mkdir -p .task-build
for unit in Camera Image Random RayTracer Scene SpatialIndex SurfacePoint Triangle Vector3f; do
 c++ -std=c++17 -O2 -I. -Isrc -c src/$unit.cpp -o .task-build/$unit.o
done
objects=(.task-build/Camera.o .task-build/Image.o .task-build/Random.o .task-build/RayTracer.o .task-build/Scene.o .task-build/SpatialIndex.o .task-build/SurfacePoint.o .task-build/Triangle.o .task-build/Vector3f.o)
if [[ $mode == target ]]; then
 test -s src/Coherent.hpp
 test -s src/Coherent.cpp
 c++ -std=c++17 -O2 -I. -Isrc -include src/Coherent.hpp -c src/Coherent.cpp -o .task-build/Coherent.o
 objects+=(.task-build/Coherent.o)
fi
c++ -std=c++17 -O2 -I. -Isrc task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/coherent.cpp
c++ -std=c++17 -O2 -I. -Isrc task_support/consumer.cpp "${objects[@]}" -o .task-build/consumer
.task-build/consumer
c++ -std=c++17 -O2 -I. -Isrc examples/coherent.cpp "${objects[@]}" -o .task-build/example
c++ -std=c++17 -O2 task_support/check_images.cpp -o .task-build/check_images
rm -f .task-build/constructive.ppm .task-build/destructive.ppm
.task-build/example .task-build
.task-build/check_images .task-build/constructive.ppm .task-build/destructive.ppm
c++ -std=c++17 -O2 -I. -Isrc task_support/driver.c tests/coherent_target.cpp "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in optics transport camera; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
