#!/usr/bin/env bash
set -euo pipefail
cmake -S . -B .baseline-build -DCMAKE_BUILD_TYPE=Debug -DMESHOPT_BUILD_DEMO=ON -DMESHOPT_BUILD_GLTFPACK=OFF -DMESHOPT_INSTALL=OFF
cmake --build .baseline-build --parallel 2
"${CXX:-c++}" -std=c++11 -O0 -g -UNDEBUG -Isrc checks/motion_baseline.cpp .baseline-build/libmeshoptimizer.a -o .baseline-build/motion-baseline
