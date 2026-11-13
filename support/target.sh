#!/bin/sh
set -eu
sh support/baseline.sh
test -s DualContouringSample/editable_octree.cpp
test -s DualContouringSample/editable_octree.h
test -s test/editable.cpp
b=$(mktemp -d "${TMPDIR:-/tmp}/dual-target.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -DCHECK_HEADLESS -Ithird_party -IDualContouringSample DualContouringSample/octree.cpp DualContouringSample/qef.cpp DualContouringSample/svd.cpp DualContouringSample/density.cpp DualContouringSample/editable_octree.cpp test/editable.cpp support/driver.cpp -o "$b/edit"
"$b/edit"
