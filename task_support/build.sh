#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DWAVPACK_BUILD_PROGRAMS=OFF -DBUILD_TESTING=OFF -DWAVPACK_ENABLE_ASM=OFF -DWAVPACK_ENABLE_THREADS=OFF -DWAVPACK_ENABLE_DSD=OFF
cmake --build .task-build --target wavpack -j 4
