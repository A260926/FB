#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
if [[ ${1:-baseline} == target ]]; then
 test -s core/spectral_transfer.h
 test -s core/spectral_transfer.cpp
 test -s materials/spectralmix.cpp
 "$cxx" -std=c++17 -Wno-narrowing -Icore -I. -include core/spectral_transfer.h -x c++ -c /dev/null -o .task-build/required_header.o
fi
for name in camera color geometry light material mc paramset primitive reflection sampling scene shape texture timer transform transport util; do
 "$cxx" -std=c++17 -Wno-narrowing -O1 -ffunction-sections -fdata-sections -Icore -I. -c "core/$name.cpp" -o ".task-build/core_$name.o"
done
for spec in 'shapes/trianglemesh.cpp:CreateShape:CreateTriangleMeshShape' 'lights/area.cpp:CreateAreaLight:CreateAreaLight' 'materials/matte.cpp:CreateMaterial:CreateMatteMaterial' 'accelerators/grid.cpp:CreateAccelerator:CreateGridAccelerator' 'accelerators/kdtree.cpp:CreateAccelerator:CreateKDTreeAccelerator' 'integrators/directlighting.cpp:CreateSurfaceIntegrator:CreateDirectIntegrator' 'integrators/path.cpp:CreateSurfaceIntegrator:CreatePathIntegrator' 'cameras/perspective.cpp:CreateCamera:CreatePerspectiveCamera'; do
 file=${spec%%:*}; rest=${spec#*:}; old=${rest%%:*}; new=${rest#*:}
 "$cxx" -std=c++17 -Wno-narrowing -O1 -ffunction-sections -fdata-sections -Icore -I. -D"$old=$new" -c "$file" -o ".task-build/${new}.o"
done
for name in fbm windy wrinkled marble; do
 "$cxx" -std=c++17 -Wno-narrowing -O1 -ffunction-sections -fdata-sections -Icore -I. -DCreateFloatTexture=Create${name}FloatTexture -DCreateSpectrumTexture=Create${name}SpectrumTexture -c "textures/$name.cpp" -o ".task-build/texture_$name.o"
done
"$cxx" -std=c++17 -Wno-narrowing -O1 -Icore -I. -c task_support/globals.cpp -o .task-build/globals.o
rm -f .task-build/libproduction.a
ar rcs .task-build/libproduction.a .task-build/core_*.o .task-build/Create*.o .task-build/texture_*.o .task-build/globals.o

# Compile every added production translation unit; the fixed source list describes the original snapshot only.
find core shapes textures materials accelerators lights integrators cameras filters samplers volumes film tonemaps -type f -name '*.cpp' | sort | while IFS= read -r file; do
 if grep -Fxq "$file" task_support/original-cpp.list; then continue; fi
 
 name=added_$(printf '%s' "$file" | tr '/.' '__')
 "$cxx" -std=c++17 -Wno-narrowing -O1 -ffunction-sections -fdata-sections -Icore -I. -c "$file" -o ".task-build/$name.o"
 ar r .task-build/libproduction.a ".task-build/$name.o"
done
