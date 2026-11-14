#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
d=$(mktemp -d "${TMPDIR:-/tmp}/bvh-baseline.XXXXXX")
trap 'rm -rf "$d"' EXIT
${CXX:-c++} -std=c++20 -O1 -pthread -Isrc checks/baseline.cpp -o "$d/base"
"$d/base"
${CXX:-c++} -std=c++20 -O1 -pthread -Isrc test/simple_example.cpp -o "$d/upstream"
"$d/upstream"
