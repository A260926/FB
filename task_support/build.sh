#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sources=(ik/src/*.c ik/src/solver/*/*.c task_support/generated/build_info.c)
objects=()
for source in "${sources[@]}"; do
  object=".task-build/$(echo "$source" | tr / _).o"
  "${CC:-cc}" -std=c11 -O1 -DIK_BUILDING -Iik/include/public -Iik/include/private -Itask_support/generated -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libik.a
ar rcs .task-build/libik.a "${objects[@]}"
"${CC:-cc}" -std=c11 -O1 -Iik/include/public -Itask_support/generated task_support/baseline.c .task-build/libik.a -lm -o .task-build/baseline
