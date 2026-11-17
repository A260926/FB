#!/bin/bash
set -euo pipefail
mkdir -p .task-build
shopt -s nullglob
objects=()
for source in src/*.cc src/*.cpp; do
  object=".task-build/$(basename "${source%.*}").o"
  c++ -std=c++11 -O1 -pthread -I. -Isrc -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libfujiyama.a
ar rcs .task-build/libfujiyama.a "${objects[@]}"
c++ -std=c++11 -O1 -pthread -I. -Isrc task_support/baseline.cpp .task-build/libfujiyama.a -o .task-build/baseline
