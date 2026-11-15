#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
for name in camera color geometry light material mc paramset primitive reflection sampling scene shape texture timer transform transport util; do
  "$cxx" -std=c++17 -Wno-narrowing -O1 -ffunction-sections -fdata-sections -Icore -I. -c "core/$name.cpp" -o ".task-build/core_$name.o"
done
for spec in 'shapes/trianglemesh.cpp:CreateShape:CreateTriangleMeshShape' 'lights/area.cpp:CreateAreaLight:CreateAreaLight' 'materials/matte.cpp:CreateMaterial:CreateMatteMaterial' 'accelerators/grid.cpp:CreateAccelerator:CreateGridAccelerator' 'accelerators/kdtree.cpp:CreateAccelerator:CreateKDTreeAccelerator' 'integrators/directlighting.cpp:CreateSurfaceIntegrator:CreateDirectIntegrator' 'integrators/path.cpp:CreateSurfaceIntegrator:CreatePathIntegrator' 'cameras/perspective.cpp:CreateCamera:CreatePerspectiveCamera'; do
  file=${spec%%:*}; rest=${spec#*:}; old=${rest%%:*}; new=${rest#*:}
  "$cxx" -std=c++17 -Wno-narrowing -O1 -ffunction-sections -fdata-sections -Icore -I. -D"$old=$new" -c "$file" -o ".task-build/${new}.o"
done
"$cxx" -std=c++17 -Wno-narrowing -O1 -Icore -I. -c task_support/globals.cpp -o .task-build/globals.o
ar rcs .task-build/libproduction.a .task-build/core_*.o .task-build/Create*.o .task-build/globals.o
