#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash checks/build.sh "$b"
c++ -std=c++11 -Isrc checks/baseline.cc "$b/libvoro++.a" -o "$b/baseline"
"$b/baseline"
c++ -std=c++11 -Isrc examples/basic/single_cell.cc "$b/libvoro++.a" -o "$b/old"
(cd "$b" && ./old && test -s single_cell.gnu)
echo VORO_OLD_SINGLE_CELL_OK
