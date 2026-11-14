#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sig=$(find src -type f \( -name '*.h' -o -name '*.cpp' \) -exec shasum -a 256 {} + | LC_ALL=C sort | shasum -a 256)
if [[ ! -f .task-build/signature || $(cat .task-build/signature) != "$sig" ]]; then
  objects=()
  for file in src/cal3d/*.cpp src/normal_encoding/normal_encoding.cpp; do
    case "$file" in */compactmeshwriter.cpp|*/PCH.cpp) continue;; esac
    obj=.task-build/$(basename "$file" .cpp).o
    c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Idevcheck/deps/rapidxml -include algorithm -include cstring -c "$file" -o "$obj"
    objects+=("$obj")
  done
  rm -f .task-build/libcal3d.a
  ar rcs .task-build/libcal3d.a "${objects[@]}"
  printf '%s' "$sig" > .task-build/signature
fi
for spec in 'task_support/baseline.cpp pipeline-baseline' 'devcheck/current.cpp compression-baseline'; do
  read -r file name <<< "$spec"
  if [[ ! -f .task-build/$name || $file -nt .task-build/$name || .task-build/libcal3d.a -nt .task-build/$name ]]; then
    c++ -std=c++17 -O0 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -include algorithm -include cstring "$file" .task-build/libcal3d.a -o .task-build/$name
  fi
done
