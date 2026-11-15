#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DMESHOPT_BUILD_DEMO=OFF -DMESHOPT_BUILD_GLTFPACK=OFF -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS_DEBUG=-O2
cmake --build .task-build/cmake --target meshoptimizer -j 2
c++ -x c++ -std=c++11 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O2 -I. task_support/baseline.cpp demo/tests.cpp .task-build/cmake/libmeshoptimizer.a -o .task-build/baseline
