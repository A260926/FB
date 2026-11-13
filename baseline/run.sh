#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p .baseline-build
"${CXX:-c++}" -std=c++17 -O1 -Wall -Wextra src/pugixml.cpp baseline/original.cpp -o .baseline-build/original
.baseline-build/original
