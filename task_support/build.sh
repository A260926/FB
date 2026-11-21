#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
sources=(task_support/baseline.cpp)
if [[ -e volume_sampling.cpp ]]; then
  test -s volume_sampling.cpp
  sources+=(volume_sampling.cpp)
fi
c++ -std=c++17 -O2 -I. "${sources[@]}" -o .task-build/baseline > .task-build/baseline-build.log 2>&1 || { tail -30 .task-build/baseline-build.log; exit 1; }
