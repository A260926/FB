#!/bin/bash
set -euo pipefail
src=${1:-harness/baseline.cpp}
if [ "$#" -gt 0 ]; then shift; fi
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O1 -I. QuickHull.cpp "$src" -o "$b/check"
"$b/check" "$@"
