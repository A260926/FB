#!/bin/sh
set -eu

# build in an explicit temp directory (mktemp needs a writable template dir)
build=$(mktemp -d "${TMPDIR:-/tmp}/msaa4.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

repo=$(cd "$(dirname "$0")/.." && pwd)
ppm="$repo/harness/msaa4_quarter_edge.ppm"

cxxflags="-std=c++20 -O2 -I$repo/rasterizer"
srcs="$repo/rasterizer/renderer.cpp $repo/rasterizer/math/utility.cpp"

# old baseline must keep passing
c++ $cxxflags "$repo/harness/baseline.cpp" $srcs -o "$build/baseline"
"$build/baseline"

# fixed 4-sample pipeline tests, with the small edge PPM
c++ $cxxflags "$repo/harness/msaa4_test.cpp" $srcs -o "$build/msaa4_test"
"$build/msaa4_test" "$ppm"

echo "ppm written to: $ppm"
