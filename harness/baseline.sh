#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S harness -B "$b" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$b" --target boundary_baseline -j 4
"$b/boundary_baseline"
