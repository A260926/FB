#!/bin/bash
set -euo pipefail
mode=${1:-baseline}
mkdir -p .task-build
cxx=${CXX:-c++}
flags=(-std=c++20 -O0 -DPMP_SCALAR_TYPE_64 -Isrc -Iexternal/eigen-5.0.1 -I.)
if [[ $mode == target ]]; then
 test -s src/pmp/algorithms/subdivision_plan.h
 test -s src/pmp/algorithms/subdivision_plan.cpp
 "$cxx" "${flags[@]}" -include src/pmp/algorithms/subdivision_plan.h -c src/pmp/algorithms/subdivision_plan.cpp -o .task-build/plan.o
fi
for file in src/pmp/surface_mesh.cpp src/pmp/algorithms/subdivision.cpp src/pmp/algorithms/differential_geometry.cpp src/pmp/algorithms/normals.cpp src/pmp/algorithms/shapes.cpp; do
 name=${file//\//_}
 "$cxx" "${flags[@]}" -c "$file" -o ".task-build/$name.o"
done
# Only this support entry owns the object directory. Avoid archive members from older runs.
rm -f .task-build/libproduction.a
ar rcs .task-build/libproduction.a .task-build/src_pmp_*.o
if [[ $mode == target ]]; then ar r .task-build/libproduction.a .task-build/plan.o; fi

# Compile every added production translation unit; the fixed source list describes the original snapshot only.
find src/pmp -type f -name '*.cpp' | sort | while IFS= read -r file; do
 if grep -Fxq "$file" task_support/original-cpp.list; then continue; fi
 if [[ "$file" == src/pmp/algorithms/subdivision_plan.cpp ]]; then continue; fi
 name=added_$(printf '%s' "$file" | tr '/.' '__')
 "$cxx" "${flags[@]}" -c "$file" -o ".task-build/$name.o"
 ar r .task-build/libproduction.a ".task-build/$name.o"
done
