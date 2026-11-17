#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in src/*.cpp; do
  case "$source" in src/Example.cpp|src/Sample.cpp) continue;; esac
  object=".task-build/$(basename "${source%.cpp}").o"
  c++ -std=c++17 -O1 -pthread -I. -Itask_support -Isrc -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libjet.a
ar rcs .task-build/libjet.a "${objects[@]}"
c++ -std=c++17 -O1 -pthread -I. -Itask_support -Isrc tests/mesh_instances.cpp .task-build/libjet.a -o .task-build/baseline
