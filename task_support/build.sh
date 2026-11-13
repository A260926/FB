#!/usr/bin/env bash
set -euo pipefail
mode=$1
out=$2
mkdir -p "$out"
cc -std=c99 -O1 -Isrc -c src/nanovg.c -o "$out/nanovg.o"
if [ "$mode" = baseline ]; then
 cc -std=c99 -O1 -Isrc task_support/baseline.c "$out/nanovg.o" -lm -o "$out/baseline"
else
 if [ -f src/nanovg_cpu.c ]; then cc -std=c99 -O1 -Isrc -c src/nanovg_cpu.c -o "$out/cpu.o"; fi
 c++ -std=c++17 -O1 -Isrc -Itask_support task_support/main.cpp task_tests/target.cpp "$out"/*.o -lm -o "$out/target"
fi
