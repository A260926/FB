#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/iir-base.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O1 -I. checks/baseline.cpp iir/*.cpp -o "$b/base"
"$b/base"
c++ -std=c++17 -O1 -I. test/custom.cpp iir/*.cpp -o "$b/custom"
"$b/custom"
echo IIR_OLD_CUSTOM_OK
