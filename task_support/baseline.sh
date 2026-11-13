#!/usr/bin/env bash
set -euo pipefail
mkdir -p .baseline
${CXX:-c++} -std=c++11 -O0 -pthread -Iinclude -Isrc task_support/baseline.cpp src/*.cpp src/detail/*.cpp src/gen/*.cpp src/gen/detail/*.cpp -o .baseline/native_observer
.baseline/native_observer
