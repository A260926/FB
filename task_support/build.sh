#!/bin/bash
set -euo pipefail
mkdir -p .task-build/codec .task-build/utility
rm -f .task-build/codec/*.o .task-build/utility/*.o
for source in codec/*.c codec/*.cpp; do
 [[ -f "$source" ]] || continue
 base=$(basename "$source")
 c++ -std=c++11 -O1 -Icodec -x c++ -c "$source" -o ".task-build/codec/$base.o"
done
for source in convert-utility/*.c convert-utility/*.cpp; do
 [[ -f "$source" ]] || continue
 [[ "$source" == convert-utility/main.cpp ]] && continue
 base=$(basename "$source")
 c++ -std=c++11 -O1 -Icodec -Iconvert-utility -x c++ -c "$source" -o ".task-build/utility/$base.o"
done
c++ -std=c++11 -O1 -Icodec -Iconvert-utility convert-utility/main.cpp .task-build/codec/*.o .task-build/utility/*.o -o .task-build/alacconvert
c++ -std=c++11 -O1 -Icodec -Iconvert-utility task_support/baseline.cpp .task-build/codec/*.o .task-build/utility/*.o -o .task-build/baseline
