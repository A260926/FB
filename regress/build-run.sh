#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O1 -I. "${1:-regress/baseline.cpp}" -o "$b/check"
"$b/check"
