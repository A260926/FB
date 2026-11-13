#!/usr/bin/env bash
set -euo pipefail
mode=$1
out=$2
mkdir -p "$out"
if [ "$mode" = baseline ]; then
 c++ -std=c++17 -O1 -Isrc task_support/baseline.cpp -o "$out/baseline"
 c++ -std=c++17 -O1 test/test.cpp -o "$out/upstream_tests"
else
 c++ -std=c++17 -O1 -Isrc -Itask_support task_support/main.cpp task_tests/target.cpp -o "$out/target"
fi
