#!/bin/bash
set -euo pipefail
mkdir -p .task-build
signature=$(find src/pmp task_support -type f ! -name run.sh -exec cksum {} + | cksum)
if [[ ! -f .task-build/build.signature ]] || [[ $(cat .task-build/build.signature) != "$signature" ]]; then
  objects=()
  sources=(src/pmp/surface_mesh.cpp src/pmp/algorithms/normals.cpp)
  while IFS= read -r source; do sources+=("$source"); done < <(find src/pmp/io -name '*.cpp' | LC_ALL=C sort)
  while IFS= read -r source; do
    if ! grep -Fqx "$source" task_support/upstream-algorithms.txt; then sources+=("$source"); fi
  done < <(find src/pmp/algorithms -name '*.cpp' | LC_ALL=C sort)
  for source in "${sources[@]}"; do
    object=".task-build/$(echo "$source" | tr / _).o"
    c++ -std=c++20 -O0 -w -Isrc -Iexternal/eigen-5.0.1 -c "$source" -o "$object"
    objects+=("$object")
  done
  rm -f .task-build/core.a
  ar rcs .task-build/core.a "${objects[@]}"
  c++ -std=c++20 -O0 -w -Isrc -Iexternal/eigen-5.0.1 task_support/baseline.cpp .task-build/core.a -o .task-build/baseline
  printf '%s\n' "$signature" > .task-build/build.signature
fi
