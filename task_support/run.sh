#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
if [[ ${1:-baseline} == target ]]; then
  test -s tests/type_layout_cases.cpp || { echo 'MISSING tests/type_layout_cases.cpp' >&2; exit 2; }
  c++ -std=c++17 -I. tests/type_layout_cases.cpp -o "$out/layout"
  "$out/layout" | tee "$out/target.log"
  for group in units recursion layout; do grep -qx "PASS $group" "$out/target.log"; done
  grep -qx 'TYPE_LAYOUT_OK' "$out/target.log"
fi
c++ -std=c++17 -I. task_support/baseline.cpp -o "$out/baseline"
"$out/baseline"
c++ -std=c++17 -I. examples/tutorial/tutorial.cpp -o "$out/tutorial"
"$out/tutorial" tests/elf_examples/asm64 | head -30
