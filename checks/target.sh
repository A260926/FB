#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test -s checks/target.cpp
test -s examples/uv_baker/baker.h
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
${CXX:-c++} -std=c++17 -O1 -I. -Iexamples/uv_baker checks/target.cpp examples/uv_baker/baker.cpp -o "$d/target"
"$d/target" "$d"
sh checks/baseline.sh
