#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
printf '*\n' > .task-build/.gitignore
cmake -S . -B .task-build/wabt -DBUILD_TESTS=OFF -DBUILD_LIBWASM=OFF \
  -DWITH_WASI=OFF -DUSE_INTERNAL_SHA256=ON -DCMAKE_BUILD_TYPE=Release \
  >.task-build/configure.log 2>&1 || { cat .task-build/configure.log; exit 1; }
cmake --build .task-build/wabt --target wat2wasm wasm-objdump --parallel 4 \
  >.task-build/build.log 2>&1 || { cat .task-build/build.log; exit 1; }
