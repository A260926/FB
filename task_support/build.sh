#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
cxx=${CXX:-c++}
: > .task-build/objects.list
compile_source() {
  source=$1
  obj=.task-build/obj/$(echo "$source" | tr / _).o
  if test ! -f "$obj" || test "$source" -nt "$obj" || find src extlib/eigen/Eigen -name '*.h' -newer "$obj" -print -quit | read -r changed; then
    "$cxx" -std=c++17 -O1 -w -ffunction-sections -fdata-sections -Isrc -Iextlib/eigen -Itask_support -c "$source" -o "$obj"
  fi
  printf '%s\n' "$obj" >> .task-build/objects.list
}
for source in src/util.cpp src/polygon.cpp src/mesh.cpp src/polyline.cpp src/groupmesh.cpp src/export.cpp src/srf/*.cpp task_support/host.cpp; do
  compile_source "$source"
done
find src -name '*.cpp' -type f | LC_ALL=C sort > .task-build/current-cpp.list
while IFS= read -r source; do
  if ! grep -Fxq "$source" task_support/upstream-cpp.txt; then
    case "$source" in src/srf/*) ;; *) compile_source "$source" ;; esac
  fi
done < .task-build/current-cpp.list
# Other callers of the changed shell entry must still compile. No sketch/UI is run.
"$cxx" -std=c++17 -w -Isrc -Iextlib/eigen -Itask_support -fsyntax-only src/solvespace.cpp
