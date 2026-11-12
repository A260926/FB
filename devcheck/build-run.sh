#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O1 -Iinclude -Iexternal/eigen "${1:-devcheck/baseline.cpp}" -o "$b/check"
"$b/check"
