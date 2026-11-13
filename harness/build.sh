#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .build
c++ -std=c++17 -O1 -g -Iinclude -Isrc -c src/simdjson.cpp -o .build/simdjson.o
