#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s checks/target.cpp
test -s src/renderer/DepthPeeler.h
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CXX:-c++} -std=c++17 -O1 -Wno-unknown-pragmas -Isrc/renderer checks/target.cpp src/renderer/*.cpp -o "$d/target"
"$d/target" "$d"
sh checks/baseline.sh
