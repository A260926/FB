#!/bin/bash
set -euo pipefail
b=${CHECK_BUILD_DIR:-$(mktemp -d)}
bash support/compile.sh "$b"
lib=$(find "$b" -name libspatialindex.a -type f)
c++ -std=c++11 -Iinclude -Isupport support/target_main.cpp tests/nearest_join.cpp "$lib" -o "$b/target"
"$b/target" | tee "$b/target.log"
for group in results storage pruning; do
 grep -Eq "^JOIN_GROUP $group PASS cases=[1-9][0-9]* checks=[1-9][0-9]*$" "$b/target.log"
done
CHECK_BUILD_DIR="$b" bash support/baseline.sh
