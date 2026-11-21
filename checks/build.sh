#!/bin/bash
set -euo pipefail
mkdir -p .task-build
c++ -std=c++14 -O2 -pthread -I. -Iinclude checks/baseline.cpp -o .task-build/baseline
