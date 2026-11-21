#!/bin/bash
set -euo pipefail
mkdir -p .task-build
make -f task_support/build.mk -j4 archive
c++ -std=c++17 -Isrc -I. task_support/baseline.cpp .task-build/libproduction.a -o .task-build/baseline
