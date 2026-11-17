#!/bin/bash
set -euo pipefail
mkdir -p .task-build
for backend in USE_SCALAR USE_PACKED_SCALAR; do
  c++ -std=c++17 -O1 -pthread -D"$backend" -Isrc -Idemos -I. task_support/baseline.cpp demos/demo_new.cpp -o ".task-build/baseline-$backend"
done
