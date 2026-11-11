#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
build=$1
cmake -S "$root" -B "$build" -DCAPSTONE_ARCHITECTURE_DEFAULT=OFF -DCAPSTONE_ARM64_SUPPORT=ON -DCAPSTONE_BUILD_MACOS_THIN=ON -DCAPSTONE_BUILD_TESTS=ON -DCAPSTONE_BUILD_CSTOOL=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Debug > "$build/configure.log" 2>&1
cmake --build "$build" --target capstone_static test_arm64 -j4 > "$build/build.log" 2>&1
ctest --test-dir "$build" -R '^capstone_test_arm64$' --output-on-failure
