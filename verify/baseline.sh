#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O2 -I. verify/baseline.c -lm -o "$build/check"
"$build/check"
cc -std=c99 -Itest -c test/console-colors.c -o "$build/colors.o"
c++ -std=c++17 -O2 -I. -Itest test/test_octasphere.cpp "$build/colors.o" -o "$build/upstream"
(cd "$build" && ./upstream && test -s octasphere.gltf && test -s octasphere.bin)
echo 'BASELINE upstream octasphere examples PASS'
