#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s checks/target.cpp
test -s src/bvh/v2/light_tree.h
test -s examples/light_tree_preview.cpp
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CXX:-c++} -std=c++20 -O1 -pthread -Isrc checks/target.cpp -o "$d/target"
"$d/target" "$d"
${CXX:-c++} -std=c++20 -O1 -pthread -Isrc examples/light_tree_preview.cpp -o "$d/preview"
"$d/preview" "$d/light-tree.ppm"
test -s "$d/light-tree.ppm"
sh checks/baseline.sh
