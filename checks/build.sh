#!/bin/bash
set -euo pipefail
mkdir -p .task-build
prod_sources=(src/camera/camera.cpp src/light/light.cpp src/ray_tracer/objects.cpp src/ray_tracer/ray_tracer.cpp)
while IFS= read -r path; do
  case "$path" in
    src/camera/camera.cpp|src/light/light.cpp|src/ray_tracer/objects.cpp|src/ray_tracer/ray_tracer.cpp|src/demo*.cpp) ;;
    *) prod_sources+=("$path") ;;
  esac
done < <(find src -type f -name '*.cpp' | LC_ALL=C sort)
c++ -std=c++17 -O2 -Wno-c++20-extensions -Wno-unknown-attributes -Isrc/common -Isrc/math -Isrc/light -Isrc/camera -Isrc/ppm_writer -Isrc/ray_tracer -Isrc/incremental -I. checks/baseline.cpp "${prod_sources[@]}" -o .task-build/baseline
