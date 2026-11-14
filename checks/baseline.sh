#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
sources=(src/renderer/VertexProcessor.cpp src/renderer/PolyClipper.cpp src/renderer/LineClipper.cpp)
for part in clipping interpolation; do
  "${CXX:-c++}" -std=c++17 -O2 -Isrc/renderer "checks/$part.cpp" "${sources[@]}" -o ".task-build/$part"
  ".task-build/$part"
done
"${CXX:-c++}" -std=c++17 -O2 -Isrc/renderer -Ichecks/headless src/examples/VertexProcessorTest.cpp "${sources[@]}" -o .task-build/upstream-example
.task-build/upstream-example
