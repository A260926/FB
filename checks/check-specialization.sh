#!/bin/sh
# Runs the partial-specialization checks, the original baseline and the
# existing CTest runner. Uses only the in-tree C/C++ and CMake toolchain; no
# graphics driver or extra SDK is required (the numeric checks use the CPU NIR
# execution bridge in tests/nir_interp.h).
set -eu

cmake -S . -B build -DBUILD_TESTING=ON -DGLSLOPT_TEST_OPENGL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4

# Original baseline: enabled and weights[1] stay ordinary uniforms.
c++ -std=c++17 -Isrc/glsl checks/baseline.cpp build/libglsl_optimizer.a -pthread -o build/baseline
./build/baseline

# New specialization C++ suite (partial arrays, struct fields, helpers,
# side-effecting indices, dead-texture removal, validation, compatibility).
./build/glslopt_specialization_tests

# Existing fixture runner, through the registered CTest entry point.
ctest --test-dir build -R test_runner --output-on-failure

echo "CHECK_SPECIALIZATION_OK"
