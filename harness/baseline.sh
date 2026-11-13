#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bash harness/build.sh
c++ -std=c++17 -O1 -Iinclude harness/baseline.cpp .build/simdjson.o -pthread -o .build/baseline
.build/baseline
