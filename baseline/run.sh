#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
for mode in narrow wide; do
  flags=(-DPUGIXML_MEMORY_OUTPUT_STACK=128)
  if [[ "$mode" == wide ]]; then flags+=(-DPUGIXML_WCHAR_MODE); fi
  clang++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${flags[@]}" -Isrc src/pugixml.cpp baseline/witness.cpp -o "$build/witness"
  "$build/witness"
  echo "PUGI_${mode}_BASELINE_PASS"
done
