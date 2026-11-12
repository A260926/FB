#!/usr/bin/env bash
set -euo pipefail
file=${1:?C++ driver required}
test -s "$file"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O2 -Isrc "$file" \
 src/OpenMesh/Core/Mesh/ArrayKernel.cc \
 src/OpenMesh/Core/Mesh/PolyConnectivity.cc \
 src/OpenMesh/Core/Mesh/TriConnectivity.cc \
 src/OpenMesh/Core/Mesh/BaseKernel.cc \
 src/OpenMesh/Core/System/omstream.cc \
 src/OpenMesh/Core/Utils/BaseProperty.cc -o "$build/check"
"$build/check"
