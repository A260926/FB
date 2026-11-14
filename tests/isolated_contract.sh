#!/usr/bin/env bash
# Builds and runs the isolated checking contract tests, then the local
# usage example. Mirrors task_support/baseline.sh: C++11, no test frameworks.
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p .isolated
CXX="${CXX:-c++}"
SRC="src/*.cpp src/detail/*.cpp src/gen/*.cpp src/gen/detail/*.cpp"

$CXX -std=c++11 -O0 -pthread -Iinclude -Isrc -Itests \
  tests/isolated_contract.cpp $SRC -o .isolated/isolated_contract
.isolated/isolated_contract | tee .isolated/contract.log

# The contract binary only exits 0 after printing every marker; verify them
# explicitly as well.
grep -q '^ISOLATED_RESULTS_OK$' .isolated/contract.log
grep -q '^ISOLATED_SHRINK_OK$' .isolated/contract.log
grep -q '^ISOLATED_REPLAY_OK$' .isolated/contract.log
grep -q '^TLV_BOUNDARY_OK$' .isolated/contract.log

$CXX -std=c++11 -O0 -pthread -Iinclude -Isrc \
  examples/isolated/main.cpp $SRC -o .isolated/isolated_example
.isolated/isolated_example
