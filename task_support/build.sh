#!/usr/bin/env bash
set -euo pipefail
mode=$1
out=$2
mkdir -p "$out"
for src in Source/*.c; do
 cc -std=c99 -O1 -IInclude -ISource -c "$src" -o "$out/$(basename "$src" .c).o"
done
if [ "$mode" = baseline ]; then
 cc -std=c99 -O1 -IInclude task_support/baseline.c "$out"/*.o -lm -o "$out/baseline"
else
 c++ -std=c++17 -O1 -IInclude -Itask_support task_support/main.cpp task_tests/target.cpp "$out"/*.o -lm -o "$out/target"
fi
