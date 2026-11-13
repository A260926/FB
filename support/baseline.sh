#!/bin/sh
set -eu
b=$(mktemp -d "${TMPDIR:-/tmp}/dual-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -DCHECK_HEADLESS -Ithird_party -IDualContouringSample DualContouringSample/octree.cpp DualContouringSample/qef.cpp DualContouringSample/svd.cpp DualContouringSample/density.cpp support/baseline.cpp -o "$b/base"
"$b/base"
