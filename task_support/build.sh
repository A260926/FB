#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cmake -S . -B .build -DLEXBOR_BUILD_SHARED=OFF -DLEXBOR_BUILD_STATIC=ON -DLEXBOR_BUILD_EXAMPLES=OFF -DLEXBOR_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build .build --parallel 4
