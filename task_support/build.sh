#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
sources=(src/render.cpp src/scene.cpp src/mesh.cpp src/platform.cpp)
if [[ ${1:-baseline} == target ]]; then
  test -s src/heightfield.h
  test -s src/heightfield.cpp
  sources+=(src/heightfield.cpp)
fi
objects=()
for src in "${sources[@]}"; do
  obj=".task-build/$(basename "$src").o"
  if [[ ${1:-baseline} == target ]]; then
    "$cxx" -std=c++17 -O1 -DTINSEL_TARGET_BUILD -I. -Isrc -include task_support/compat.h -include src/heightfield.h -c "$src" -o "$obj"
  else
    "$cxx" -std=c++17 -O1 -I. -Isrc -include task_support/compat.h -c "$src" -o "$obj"
  fi
  objects+=("$obj")
done
rm -f .task-build/production.a
ar rcs .task-build/production.a "${objects[@]}"
"$cxx" -std=c++17 -O1 -I. -Isrc -include task_support/compat.h task_support/baseline.cpp .task-build/production.a -o .task-build/baseline
