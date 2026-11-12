#!/bin/bash
set -euo pipefail
b=$1
cmake -S . -B "$b" -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$b" --target spatialindex -j2
