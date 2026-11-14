#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cc -std=c99 -O0 -g checks/baseline.c -lm -o "$b/baseline"
"$b/baseline"
c++ -std=c++11 test/test_math.cpp -o "$b/math"
"$b/math"
echo CGLTF_OLD_MATH_OK
