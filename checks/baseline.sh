#!/bin/sh
set -eu
cmake -S . -B build -DBUILD_TESTING=ON -DGLSLOPT_TEST_OPENGL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
c++ -std=c++17 -Isrc/glsl checks/baseline.cpp build/libglsl_optimizer.a -pthread -o build/baseline
./build/baseline
