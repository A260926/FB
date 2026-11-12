#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
for src in src/*.c; do cc -std=c99 -O1 -Iinclude -Isrc -c "$src" -o "$build/$(basename "$src" .c).o"; done
c++ -std=c++17 -O1 -Iinclude checks/harness.cpp checks/target.cpp "$build"/*.o -o "$build/target"
"$build/target" | tee "$build/target.log"
for group in wide_layout repeated_widths cursor_followup buffer_edits; do
 grep -Fxq "PASS $group" "$build/target.log"
done
grep -Eq "^TARGET_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]*$" "$build/target.log"
bash checks/baseline.sh
