#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libtask.a
flags=(-std=c++20 -O1 -pthread -DCUBIQUITY_USE_POOLSTL -I. -Isrc/library -Isrc/application/external)
for src in src/library/*.cpp; do
 c++ "${flags[@]}" -c "$src" -o ".task-build/$(basename "$src" .cpp).o"
done
ar rcs .task-build/libtask.a .task-build/*.o
c++ "${flags[@]}" task_support/baseline.cpp .task-build/libtask.a -o .task-build/baseline
