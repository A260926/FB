#!/bin/bash
set -euo pipefail
compiler=${CXX:-c++}
flags=(-std=c++23 -O0 -g0 -DNDEBUG -I. -ffunction-sections -fdata-sections)
if [[ $(uname -s) == Darwin ]]; then task_link=(-Wl,-dead_strip); else task_link=(-Wl,--gc-sections); fi
objects=(.task-build/Mesh.o .task-build/GMesh.o .task-build/MeshOp.o .task-build/Geometry.o .task-build/GeomOp.o .task-build/Polygon.o .task-build/Hh.o .task-build/Hh_init.o .task-build/FileIO.o .task-build/Stat.o .task-build/Timer.o .task-build/Random.o .task-build/A3dStream.o)
while IFS= read -r file; do
 case "$file" in libHh/Mesh.cpp|libHh/GMesh.cpp|libHh/MeshOp.cpp|libHh/Geometry.cpp|libHh/GeomOp.cpp|libHh/Polygon.cpp|libHh/Hh.cpp|libHh/Hh_init.cpp|libHh/FileIO.cpp|libHh/Stat.cpp|libHh/Timer.cpp|libHh/Random.cpp|libHh/A3dStream.cpp|libHh/precompiled_libHh.cpp) continue;; esac
 if ! grep -Fqx "$file" task_support/original-cpp.txt; then
  obj=".task-build/extra-$(basename "$file").o"
  "$compiler" "${flags[@]}" -c "$file" -o "$obj"; objects+=("$obj")
 fi
done < <(find libHh -name '*.cpp' -type f | LC_ALL=C sort)
"$compiler" "${flags[@]}" "${task_link[@]}" -x c++ task_support/driver.c test/DiskAtlas_task_test.cpp -x none "${objects[@]}" -o .task-build/target
test -s examples/disk_atlas.cpp
"$compiler" "${flags[@]}" "${task_link[@]}" examples/disk_atlas.cpp "${objects[@]}" -o .task-build/example
.task-build/example
