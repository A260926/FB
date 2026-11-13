#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CXX:-c++} -std=c++17 -O1 -Wno-unknown-pragmas -Isrc/renderer checks/baseline.cpp src/renderer/*.cpp -o "$d/base"
"$d/base"
