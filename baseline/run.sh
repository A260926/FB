#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/elfio_baseline_witness.XXXXXX")
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O0 -I. baseline/witness.cpp -o "$build/witness"
"$build/witness"
