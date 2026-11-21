#!/bin/bash
set -euo pipefail
test -s src/incremental/ray_cache.hpp
test -s src/incremental/ray_cache.cpp
test -s src/incremental/query_index.hpp
test -s src/incremental/query_index.cpp
test -s examples/incremental.cpp
prod_sources=(src/camera/camera.cpp src/light/light.cpp src/ray_tracer/objects.cpp src/ray_tracer/ray_tracer.cpp)
while IFS= read -r path; do
  case "$path" in
    src/camera/camera.cpp|src/light/light.cpp|src/ray_tracer/objects.cpp|src/ray_tracer/ray_tracer.cpp|src/demo*.cpp) ;;
    *) prod_sources+=("$path") ;;
  esac
done < <(find src -type f -name '*.cpp' | LC_ALL=C sort)
cxx_flags=(-std=c++17 -O2 -Wno-c++20-extensions -Wno-unknown-attributes -I. -Isrc/common -Isrc/math -Isrc/light -Isrc/camera -Isrc/ppm_writer -Isrc/ray_tracer -Isrc/incremental)
c++ "${cxx_flags[@]}" -include src/incremental/ray_cache.hpp -include src/incremental/query_index.hpp -x c++ task_support/driver.c task_tests/incremental.cpp "${prod_sources[@]}" -o .task-build/target
c++ "${cxx_flags[@]}" examples/incremental.cpp "${prod_sources[@]}" -o .task-build/incremental
out_dir=$(mktemp -d)
trap 'rm -rf "$out_dir"' EXIT
.task-build/incremental "$out_dir"
test -s "$out_dir/before.ppm"
test -s "$out_dir/after.ppm"
