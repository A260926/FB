#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
flag=OFF
if [[ $mode == target ]]; then
  flag=ON
  for file in include/embree4/rtcore_photon_beam.h kernels/common/photon_beam.cpp kernels/bvh/bvh_photon_beam.cpp tutorials/photon_beams/renderer.h tutorials/photon_beams/renderer.cpp examples/photon_beams.cpp; do test -s "$file"; done
fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET="$flag" -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
if [[ $mode == target ]]; then
  # Only task-generated outputs are removed, so rapid source rewrites cannot reuse stale objects.
  if [[ -d .task-build ]]; then
    find .task-build -type f \( -name 'beams.cpp.o' -o -name 'renderer.cpp.o' -o -name 'photon_beam.cpp.o' -o -name 'bvh_photon_beam.cpp.o' -o -name 'photon_beams.cpp.o' -o -name 'driver.c.o' -o -name 'api.c.o' \) -delete
  fi
  rm -f .task-build/target .task-build/taskexample
  cmake --build .task-build --parallel 4 --target taskbaseline tasktarget taskexample
  for file in kernels/common/photon_beam.cpp kernels/bvh/bvh_photon_beam.cpp; do
    grep -F "$file" .task-build/compile_commands.json > /dev/null
  done
  ar -t .task-build/core/libembree4.a | grep -E "(^|/)photon_beam.cpp.o$" > /dev/null
  ar -t .task-build/core/libembree4.a | grep -E "(^|/)bvh_photon_beam.cpp.o$" > /dev/null
else
  cmake --build .task-build --parallel 4 --target taskbaseline
fi
