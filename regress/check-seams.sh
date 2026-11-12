#!/usr/bin/env bash
# Compile the seam-cut regression driver against the actual library sources
# in a temporary directory and run all named scenarios.
set -euo pipefail
cd "$(dirname "$0")/.."
test -s regress/seam-cut.cpp
test -s src/OpenMesh/Tools/Utils/SeamCutT.hh
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -DOM_INCLUDE_TEMPLATES -O0 -Wno-deprecated-declarations -Isrc \
  regress/seam-cut.cpp \
  src/OpenMesh/Core/Mesh/ArrayKernel.cc \
  src/OpenMesh/Core/Mesh/BaseKernel.cc \
  src/OpenMesh/Core/Mesh/PolyConnectivity.cc \
  src/OpenMesh/Core/Mesh/TriConnectivity.cc \
  src/OpenMesh/Core/System/omstream.cc \
  src/OpenMesh/Core/Utils/BaseProperty.cc \
  -o "$build/seam-cut"
"$build/seam-cut"
