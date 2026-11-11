#!/usr/bin/env bash
set -euo pipefail
source_file=${1:?C++ source path required}
test -s "$source_file"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -DOM_INCLUDE_TEMPLATES -O0 -Wno-deprecated-declarations -Isrc "$source_file" src/OpenMesh/Core/Mesh/ArrayKernel.cc src/OpenMesh/Core/Mesh/BaseKernel.cc src/OpenMesh/Core/Mesh/PolyConnectivity.cc src/OpenMesh/Core/Mesh/TriConnectivity.cc src/OpenMesh/Core/System/omstream.cc src/OpenMesh/Core/Utils/BaseProperty.cc -o "$build/check"
"$build/check"
