#!/bin/bash
set -euo pipefail
mkdir -p checks/.tmp
b=$(mktemp -d checks/.tmp/baseline.XXXXXX)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -Dsisl_COMPILE_APPS=OFF -Dsisl_COMPILE_EXAMPLES=OFF -Dsisl_COMPILE_VIEWER=OFF >"$b/configure.log" 2>&1 || { cat "$b/configure.log"; exit 1; }
cmake --build "$b" --target sisl -j 8 >"$b/build.log" 2>&1 || { cat "$b/build.log"; exit 1; }
cc -std=c99 -Iinclude "${1:-checks/baseline.c}" "$b/libsisl.a" -lm -o "$b/check"
"$b/check"
