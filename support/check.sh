#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s support/roll_mix_test.c
test -s support/roll_mix_cpp_test.cpp
bash support/baseline.sh
cmake --build .build --target regression cpp_regression -j 4
.build/out/bin/regression
.build/out/bin/cpp_regression
