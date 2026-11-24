#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O1 -DNDEBUG' -DBUILD_SHARED_LIBS=OFF -DENABLE_SIMD=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DENABLE_SDL=OFF -DENABLE_SHERLOCK265=OFF >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build --target de265 --clean-first -j4 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
c++ -std=c++17 -I. -I.task-build task_support/decode_probe.cc .task-build/libde265/libde265.a -pthread -o .task-build/decode_probe
