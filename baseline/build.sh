#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
cxx=${CXX:-clang++}
objects=()
for source in src/*.cpp; do
  object=".task-build/$(basename "${source%.cpp}").o"
  "$cxx" -std=c++17 -O0 -g -I. -Isrc -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libraw_pdb.a
ar rcs .task-build/libraw_pdb.a "${objects[@]}"
"$cxx" -std=c++17 -O0 -g -I. -Isrc baseline/read_paths.cpp .task-build/libraw_pdb.a -o .task-build/baseline
