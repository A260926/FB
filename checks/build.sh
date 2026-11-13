#!/bin/bash
set -euo pipefail
out=$1; shift
c++ -std=c++17 -O0 -Isrc/renderer -I. src/renderer/VertexProcessor.cpp src/renderer/PolyClipper.cpp src/renderer/LineClipper.cpp "$@" -o "$out"
