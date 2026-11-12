#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O0 -I. baseline/witness.cpp -o "$build/witness"
"$build/witness"
