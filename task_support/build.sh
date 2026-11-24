#!/bin/bash
set -euo pipefail
mode=$1
rm -rf .task-build
mkdir -p .task-build/obj
find opensubdiv/bfr opensubdiv/far opensubdiv/sdc opensubdiv/vtr -name '*.cpp' -print | sort > .task-build/sources
if [[ $mode == target ]]; then
  for path in opensubdiv/bfr/clipSurface.h opensubdiv/bfr/clipSurface.cpp opensubdiv/bfr/trimTessellation.h opensubdiv/bfr/trimTessellation.cpp; do test -s "$path"; done
fi
# Files are upstream controlled and contain no whitespace; no empty source list.
test -s .task-build/sources
xargs -P 4 -I '{}' bash -c 'set -e; f=$1; out=.task-build/obj/${f//\//_}.o; if [[ "$f" == opensubdiv/bfr/clipSurface.cpp || "$f" == opensubdiv/bfr/trimTessellation.cpp ]]; then c++ -std=c++17 -O0 -I. -include opensubdiv/bfr/clipSurface.h -include opensubdiv/bfr/trimTessellation.h -c "$f" -o "$out"; else c++ -std=c++17 -O0 -I. -c "$f" -o "$out"; fi' _ '{}' < .task-build/sources
ar rcs .task-build/libcpu.a .task-build/obj/*.o
c++ -std=c++17 -O0 -I. task_support/baseline.cpp .task-build/libcpu.a -o .task-build/baseline
