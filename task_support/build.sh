#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in discrete_mirage/src/*.c; do
  # render_ortho.c is an upstream template included by discrete_mirage.c.
  [[ $source == */render_ortho.c ]] && continue
  object=".task-build/$(basename "$source").o"
  "${CC:-cc}" -std=gnu11 -O1 -Idiscrete_mirage/include -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libdmir.a
ar rcs .task-build/libdmir.a "${objects[@]}"
"${CXX:-c++}" -std=c++20 -O1 -Idiscrete_mirage/include task_support/baseline.cpp .task-build/libdmir.a -lm -o .task-build/baseline
