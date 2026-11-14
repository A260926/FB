#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d); trap 'rm -rf "$build"' EXIT
sources=()
for file in src/cal3d/*.cpp; do
 case "$file" in */compactmeshwriter.cpp|*/PCH.cpp) continue;; esac
 sources+=("$file")
done
c++ -std=c++17 -DUSE_CAL3D_WITH_CPP_11 -DIMVU_NO_INTRINSICS -Isrc -Idevcheck/deps/rapidxml -include algorithm -include cstring "${1:-devcheck/current.cpp}" "${sources[@]}" src/normal_encoding/normal_encoding.cpp -o "$build/current"
"$build/current"
