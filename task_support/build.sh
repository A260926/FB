#!/bin/bash
set -euo pipefail
mkdir -p .task-build
compiler=${CXX:-c++}
flags=(-std=c++23 -O0 -g0 -DNDEBUG -I. -ffunction-sections -fdata-sections)
units=(Mesh GMesh MeshOp Geometry GeomOp Polygon Hh Hh_init FileIO Stat Timer Random A3dStream PMesh SrMesh FrameIO Hh_main)
stamp=$( { cksum libHh/*.h task_support/baseline.cpp; for unit in "${units[@]}"; do cksum "libHh/$unit.cpp"; done; echo "$compiler ${flags[*]}"; } | cksum)
if [[ ! -f .task-build/stamp || $(cat .task-build/stamp) != "$stamp" ]]; then
 for unit in "${units[@]}"; do "$compiler" "${flags[@]}" -c "libHh/$unit.cpp" -o ".task-build/$unit.o"; done
 "$compiler" "${flags[@]}" -c task_support/baseline.cpp -o .task-build/baseline.o
 if [[ $(uname -s) == Darwin ]]; then task_link=(-Wl,-dead_strip); else task_link=(-Wl,--gc-sections); fi
 objects=(.task-build/baseline.o)
 for unit in "${units[@]}"; do objects+=(".task-build/$unit.o"); done
 "$compiler" "${task_link[@]}" "${objects[@]}" -o .task-build/baseline
 echo "$stamp" >.task-build/stamp
fi
