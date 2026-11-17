#!/bin/bash
set -euo pipefail
mkdir -p .task-build
export CCACHE_DISABLE=1
compiler=${CXX:-c++}
flags=(-std=c++23 -O0 -g0 -DNDEBUG -I. -ffunction-sections -fdata-sections)
cache_stamp=$( { cksum libHh/*.h libHh/Mesh.cpp libHh/GMesh.cpp libHh/MeshOp.cpp libHh/Geometry.cpp libHh/GeomOp.cpp libHh/Polygon.cpp libHh/Hh.cpp libHh/Hh_init.cpp libHh/FileIO.cpp libHh/Stat.cpp libHh/Timer.cpp libHh/Random.cpp libHh/A3dStream.cpp task_support/baseline.cpp; echo "$compiler ${flags[*]}"; } | cksum)
if [[ ! -f .task-build/stamp || $(cat .task-build/stamp) != "$cache_stamp" ]]; then
for unit in Mesh GMesh MeshOp Geometry GeomOp Polygon Hh Hh_init FileIO Stat Timer Random A3dStream; do
 "$compiler" "${flags[@]}" -c "libHh/$unit.cpp" -o ".task-build/$unit.o"
done
"$compiler" "${flags[@]}" -c task_support/baseline.cpp -o .task-build/baseline.o
if [[ $(uname -s) == Darwin ]]; then task_link=(-Wl,-dead_strip); else task_link=(-Wl,--gc-sections); fi
"$compiler" "${task_link[@]}" .task-build/baseline.o .task-build/Mesh.o .task-build/GMesh.o .task-build/MeshOp.o .task-build/Geometry.o .task-build/GeomOp.o .task-build/Polygon.o .task-build/Hh.o .task-build/Hh_init.o .task-build/FileIO.o .task-build/Stat.o .task-build/Timer.o .task-build/Random.o .task-build/A3dStream.o -o .task-build/baseline
echo "$cache_stamp" > .task-build/stamp
fi
