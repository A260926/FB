#!/bin/bash
set -euo pipefail
cmake -S . -B "$1" -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF >"$1/configure.log" 2>&1
cmake --build "$1" --target faad -j 4 >"$1/build.log" 2>&1
