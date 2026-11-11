#!/bin/sh
set -eu
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT HUP INT TERM
c++ -std=c++20 -O2 -Irasterizer author/baseline.cpp rasterizer/renderer.cpp rasterizer/math/utility.cpp -o "$build/baseline"
"$build/baseline"
