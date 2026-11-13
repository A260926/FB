#!/bin/sh
set -eu
cmake -S . -B devcheck/build -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=ON -DUSE_CRYPTO=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build devcheck/build -j4
