#!/bin/bash
set -euo pipefail
mkdir -p .task-build
shopt -s nullglob
sources=(src/*.cc src/*.cpp)
signature=$(find src task_support -type f \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name 'build.sh' \) -exec shasum -a 256 {} + | LC_ALL=C sort | shasum -a 256)
if [[ ! -f .task-build/signature || $(cat .task-build/signature) != "$signature" ]]; then
 objects=()
 for file in "${sources[@]}"; do
   obj=.task-build/$(basename "$file").o
   c++ -std=c++17 -O0 -Isrc -c "$file" -o "$obj"
   objects+=("$obj")
 done
 rm -f .task-build/libaabb.a
 ar rcs .task-build/libaabb.a "${objects[@]}"
 c++ -std=c++17 -O0 -Isrc task_support/baseline.cpp .task-build/libaabb.a -o .task-build/baseline
 printf '%s' "$signature" > .task-build/signature
fi
