#!/usr/bin/env bash
set -euo pipefail
mode=$1
out=$2
mkdir -p "$out"
cmake -S . -B "$out/lib" -DBLEND2D_NO_JIT=ON -DBLEND2D_STATIC=ON -DBLEND2D_TEST=OFF -DCMAKE_BUILD_TYPE=Release >"$out/configure.log" 2>&1 || { cat "$out/configure.log"; exit 1; }
cmake --build "$out/lib" --target blend2d -j 4 >"$out/compile.log" 2>&1 || { cat "$out/compile.log"; exit 1; }
c++ -std=c++17 -O1 -DBL_STATIC -I. task_support/baseline.cpp "$out/lib/libblend2d.a" -lpthread -o "$out/baseline"
if [ "$mode" = target ]; then
 c++ -std=c++17 -O1 -DBL_STATIC -I. -Itask_support task_support/main.cpp task_tests/target.cpp "$out/lib/libblend2d.a" -lpthread -o "$out/target"
elif [ "$mode" != baseline ]; then
 echo "unknown build mode: $mode" >&2
 exit 1
fi
