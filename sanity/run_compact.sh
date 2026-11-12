#!/usr/bin/env bash
set -euo pipefail
test -s sanity/compact_elevation.cpp
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -O2 -Isrc -c src/tinyspline.c -o "$build/spline.o"
cc -O2 -Isrc -c src/parson.c -o "$build/parson.o"
c++ -std=c++11 -O2 -Isrc sanity/compact_elevation.cpp src/tinysplinecxx.cxx "$build/spline.o" "$build/parson.o" -o "$build/check"
"$build/check" | tee "$build/output"
grep -Fq 'compact elevation PASS' "$build/output"
bash sanity/baseline.sh
