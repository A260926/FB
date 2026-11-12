#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
source checks/build-lib.sh
c++ -std=c++17 -O1 -I"$build/include" checks/harness.cpp checks/target.cpp "$build"/*.o -lm -o "$build/target"
"$build/target" | tee "$build/target.log"
for group in decoded_fragments parser_roles error_completion bounded_storage; do
 grep -Fxq "PASS $group" "$build/target.log"
done
grep -Eq "^TARGET_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]*$" "$build/target.log"
bash checks/baseline.sh
