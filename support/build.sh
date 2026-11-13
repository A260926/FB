#!/bin/bash
set -euo pipefail
cmake -S . -B "$1" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_CPACK=OFF -DENABLE_PACKAGE_CONFIG=OFF >"$1/configure.log" 2>&1
cmake --build "$1" --target sndfile -j 8 >"$1/build.log" 2>&1
