#!/bin/bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
extra=""; if [[ -f extras/xinclude.cpp ]]; then extra="extras/xinclude.cpp"; fi
c++ -std=c++17 -O1 -I. -Isrc src/pugixml.cpp ${extra:-} checks/harness.cpp checks/target.cpp -o "$build/target"
"$build/target" | tee "$build/target.log"
for group in recursive_resources selected_context text_fallback structural_errors; do
 grep -Fxq "PASS $group" "$build/target.log"
done
grep -Eq "^TARGET_COMPLETE cases=[1-9][0-9]* assertions=[1-9][0-9]*$" "$build/target.log"
bash checks/baseline.sh
