#!/bin/bash
set -euo pipefail
out=$1; shift
lib=${CHECK_CMAKE_BUILD:-$(dirname "$out")/lib}
cmake -S . -B "$lib" -DBUILD_TESTING=ON -DGLSLOPT_BUILD_TOOLS=OFF -DGLSLOPT_TEST_OPENGL=OFF -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$lib" --target glsl_optimizer glsl_optimizer_tests -j 4 >/dev/null
c++ -std=c++17 -O0 -I. -Isrc/glsl "$@" "$lib/libglsl_optimizer.a" -pthread -o "$out"
ctest --test-dir "$lib" -R '^test_runner$' --output-on-failure
