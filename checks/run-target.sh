#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -f src/pmp/algorithms/uv_optimization.cpp
test -f tests/uv_optimization_test.cpp
bash checks/install.sh
"${CXX:-c++}" -std=c++20 -O1 -DPMP_SCALAR_TYPE_64 -Isrc -Iexternal/eigen-5.0.1 checks/target_main.cpp tests/uv_optimization_test.cpp src/pmp/algorithms/uv_optimization.cpp .task-build/libpmp.a -o .task-build/uv_tests
.task-build/uv_tests
cmake --build .task-build --target gtest_runner -j2
(cd .task-build/tests && ./gtest_runner --gtest_filter='ParameterizationTest.*:SurfaceMeshTest.*')
bash checks/baseline.sh
printf "%s\n" PMP_UV_TARGET_OK
