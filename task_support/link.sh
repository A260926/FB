#!/bin/bash
set -euo pipefail
compiler=${CXX:-c++}
flags=(-std=c++23 -O0 -g0 -DNDEBUG -I. -ffunction-sections -fdata-sections)
if [[ $(uname -s) == Darwin ]]; then task_link=(-Wl,-dead_strip); else task_link=(-Wl,--gc-sections); fi
test -s libHh/SrMeshPages.h
test -s libHh/SrMeshPages.cpp
test -s examples/paged_refinement.cpp
objects=(.task-build/Mesh.o .task-build/GMesh.o .task-build/MeshOp.o .task-build/Geometry.o .task-build/GeomOp.o .task-build/Polygon.o .task-build/Hh.o .task-build/Hh_init.o .task-build/FileIO.o .task-build/Stat.o .task-build/Timer.o .task-build/Random.o .task-build/A3dStream.o .task-build/PMesh.o .task-build/SrMesh.o .task-build/FrameIO.o .task-build/Hh_main.o)
"$compiler" "${flags[@]}" -c libHh/SrMeshPages.cpp -o .task-build/SrMeshPages.o
objects+=(.task-build/SrMeshPages.o)
while IFS= read -r file; do
 case "$file" in libHh/SrMeshPages.cpp) continue;; esac
 if ! grep -Fqx "$file" task_support/original-cpp.txt; then
  obj=".task-build/extra-$(basename "$file").o"
  "$compiler" "${flags[@]}" -c "$file" -o "$obj"; objects+=("$obj")
 fi
done < <(find libHh -name '*.cpp' -type f | LC_ALL=C sort)
"$compiler" "${flags[@]}" "${task_link[@]}" -x c++ task_support/driver.c test/SrMeshPages_task_test.cpp -x none "${objects[@]}" -o .task-build/target
"$compiler" "${flags[@]}" "${task_link[@]}" examples/paged_refinement.cpp "${objects[@]}" -o .task-build/example
output_dir=$(mktemp -d)
trap 'rm -rf "$output_dir"' EXIT
.task-build/example demos/data/gcanyon-prefix.srm "$output_dir/active.m"
test -s "$output_dir/active.m"
grep -q '^Face ' "$output_dir/active.m"
