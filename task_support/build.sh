#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/pkg
PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="$PWD/.task-build/pkg" cmake -S . -B .task-build/build -DCMAKE_BUILD_TYPE=Release -DTASK_FFT_PROBE=ON >.task-build/configure.log 2>&1
cmake --build .task-build/build --target aubio -j 4 >.task-build/build.log 2>&1
