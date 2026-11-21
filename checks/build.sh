#!/bin/bash
set -euo pipefail
mkdir -p .task-build
"${CXX:-c++}" -std=c++20 -O1 -I. -Irasterizer -include algorithm -include limits -c rasterizer/renderer.cpp -o .task-build/renderer.o
"${CXX:-c++}" -std=c++20 -O1 -I. -Irasterizer -include algorithm -include limits -c rasterizer/math/utility.cpp -o .task-build/utility.o
