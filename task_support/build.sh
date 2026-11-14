#!/bin/bash
set -euo pipefail
mkdir -p .task-build
for name in lz4 plyfile tribox3; do
  "${CC:-cc}" -std=gnu99 -O2 -c "src/third-party/$name.c" -o ".task-build/$name.o"
done
for file in VoxelOctree VoxelData PlyLoader Debug Util thread/ThreadPool thread/ThreadUtils; do
  "${CXX:-c++}" -std=c++11 -O2 -pthread -Isrc -c "src/$file.cpp" -o ".task-build/${file##*/}.o"
done
if [[ -s src/VoxelEditing.cpp ]]; then
  "${CXX:-c++}" -std=c++11 -O2 -pthread -Isrc -c src/VoxelEditing.cpp -o .task-build/VoxelEditing.o
else
  rm -f .task-build/VoxelEditing.o
fi
"${CXX:-c++}" -std=c++11 -O2 -pthread -I. -Isrc task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
