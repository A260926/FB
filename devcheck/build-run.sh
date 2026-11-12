#!/bin/bash
set -euo pipefail
b=$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/cinolib-build-run.$$")
trap 'rm -rf "$b"' EXIT
mkdir -p "$b"
c++ -std=c++17 -O1 -Iinclude -Iexternal/eigen "${1:-devcheck/baseline.cpp}" -o "$b/check"
"$b/check"
