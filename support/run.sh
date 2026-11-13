#!/usr/bin/env bash
set -eu
cd "$(dirname "$0")/.."
cmake -S support -B .build -DCMAKE_BUILD_TYPE=Release
cmake --build .build --target skeleton_baseline --parallel 2
.build/skeleton_baseline
if [ "${1:-baseline}" = target ]; then
 cmake --build .build --target skeleton_target --parallel 2
 .build/skeleton_target
fi
