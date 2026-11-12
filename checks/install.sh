#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B .check-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCHARLS_BUILD_TESTS=ON -DCHARLS_BUILD_CLI=OFF -DCHARLS_BUILD_SAMPLES=OFF -DINSTALL_GTEST=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build .check-build -j "${CHECK_JOBS:-4}"
