#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O2 -I. "${1:-sanity/baseline.cpp}" -o "$b/check"
"$b/check"
