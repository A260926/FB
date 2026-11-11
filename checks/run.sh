#!/usr/bin/env bash
set -euo pipefail
file=${1:-checks/baseline.cpp}
test -s "$file"
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++11 -O1 -DWITH_NULL -Iinclude "$file" src/core/*.cpp src/backend/null/soloud_null.cpp -pthread -o "$b/check"
"$b/check"
