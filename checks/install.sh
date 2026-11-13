#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build
"${CXX:-c++}" -std=c++17 -O1 -I. -Ieigenlib checks/baseline.cpp -o .task-build/baseline
