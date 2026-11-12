#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
c++ -std=c++14 -O1 -g -Iinclude checks/baseline.cc -o "$b/baseline"
"$b/baseline"
