#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++11 -O1 -pthread -Isource/xatlas source/xatlas/xatlas.cpp author-check/baseline.cpp -o "$build/check"
"$build/check"
