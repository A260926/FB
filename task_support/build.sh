#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_DISABLE_INTRINSICS=ON -DOPUS_DEEP_PLC=OFF -DOPUS_DRED=OFF -DOPUS_OSCE=OFF
cmake --build .task-build -j 4
