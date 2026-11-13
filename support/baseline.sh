#!/bin/bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
bash support/build.sh "$b"
c++ -std=c++17 -I. support/baseline.cc "$b/libvp9-analysis.a" -o "$b/baseline"
"$b/baseline" "$@"
