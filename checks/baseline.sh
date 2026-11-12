#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
c++ -std=c++17 -O1 -Isrc src/pugixml.cpp checks/baseline.cpp -o "$build/witness"
"$build/witness"
c++ -std=c++17 -O1 -Isrc src/pugixml.cpp tests/main.cpp tests/test.cpp tests/allocator.cpp tests/writer_string.cpp tests/test_dom_traverse.cpp -o "$build/old"
"$build/old"
