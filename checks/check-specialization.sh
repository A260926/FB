#!/bin/sh
# Validation for partial uniform specialization.
#
# Runs three independent checks from a clean configure:
#   1. the new specialization tests (numeric IR-interpreter equivalence,
#      texture/branch removal, reflection, stats, diagnostics and lifecycle),
#   2. the original baseline, whose unspecialized entry point must be unchanged,
#   3. the generic runner ctest requested for every change.
set -eu

cmake -S . -B build -DBUILD_TESTING=ON -DGLSLOPT_TEST_OPENGL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4

# 1. New specialization check (also registered as the "specialization" ctest).
./build/glslopt_specialization_tests

# 2. Original baseline: enabled and weights[1] stay uniforms, old entry intact.
c++ -std=c++17 -Isrc/glsl checks/baseline.cpp build/libglsl_optimizer.a -pthread -o build/baseline
./build/baseline

# 3. Generic runner test.
ctest --test-dir build -R test_runner --output-on-failure

echo "SPECIALIZATION_CHECKS_OK"
