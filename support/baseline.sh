#!/bin/bash
set -euo pipefail
b=${CHECK_BUILD_DIR:-$(mktemp -d)}
bash support/compile.sh "$b"
lib=$(find "$b" -name libspatialindex.a -type f)
c++ -std=c++11 -Iinclude support/baseline.cpp "$lib" -o "$b/baseline"
"$b/baseline"
