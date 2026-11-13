#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DPMP_INSTALL=OFF -DPMP_BUILD_VIEWERS=OFF -DPMP_BUILD_EXAMPLES=OFF -DPMP_BUILD_TESTS=ON -DPMP_STRICT_COMPILATION=OFF -DCMAKE_CXX_FLAGS=-DPMP_SCALAR_TYPE_64
cmake --build .task-build --target pmp -j2
"${CXX:-c++}" -std=c++20 -O1 -DPMP_SCALAR_TYPE_64 -Isrc -Iexternal/eigen-5.0.1 checks/baseline.cpp .task-build/libpmp.a -o .task-build/uv_baseline
