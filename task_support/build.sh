#!/bin/bash
set -euo pipefail
mkdir -p .task-build/objects
cxx=${CXX:-c++}
objects=()
for source in src/lpc/*.cpp src/rice/*.cpp src/frame/*.cpp src/file/*.cpp src/sela/encoder.cpp src/sela/decoder.cpp; do
 object=.task-build/objects/$(basename "$source" .cpp).o
 "$cxx" -std=c++11 -O2 -pthread -I. -c "$source" -o "$object"
 objects+=("$object")
done
rm -f .task-build/libsela.a
ar rcs .task-build/libsela.a "${objects[@]}"
"$cxx" -std=c++11 -O2 -pthread -I. task_support/baseline.cpp .task-build/libsela.a -o .task-build/baseline
