#!/bin/bash
set -euo pipefail
rm -rf .task-build
mkdir -p .task-build
c++ -std=c++11 -O0 -pthread -DXATLAS_C_API=1 -I. -c source/xatlas/xatlas.cpp -o .task-build/xatlas.o
c++ -std=c++11 -O0 -pthread -I. task_support/baseline.cpp .task-build/xatlas.o -o .task-build/baseline
