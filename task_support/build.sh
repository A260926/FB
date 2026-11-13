#!/usr/bin/env bash
set -euo pipefail
mode=$1
out=$2
mkdir -p "$out"
for src in source/*.c; do
 cc -std=c11 -O1 -DPLUTOVG_BUILD_STATIC -Iinclude -Isource -c "$src" -o "$out/$(basename "$src" .c).o"
done
if [ "$mode" = baseline ]; then
 cc -std=c11 -O1 -DPLUTOVG_BUILD_STATIC -Iinclude task_support/baseline.c "$out"/*.o -lm -o "$out/baseline"
else
 c++ -std=c++17 -O1 -DPLUTOVG_BUILD_STATIC -Iinclude -Itask_support task_support/main.cpp task_tests/target.cpp "$out"/*.o -lm -o "$out/target"
fi
