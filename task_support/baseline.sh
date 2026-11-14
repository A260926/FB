#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/old-cases
for name in relocations relocations-block-types relocations-long-func-bodies symbol-tables; do
  input="test/dump/$name.txt"
  output=".task-build/old-cases/$name"
  wat_flags=(-r)
  if [[ $name == symbol-tables ]]; then wat_flags+=(--enable-all); fi
  .task-build/wabt/wat2wasm "${wat_flags[@]}" "$input" -o "$output.wasm"
  flags=(-d -r)
  case "$name" in
    relocations) flags+=(-h -x);;
    relocations-block-types|symbol-tables) flags+=(-x);;
  esac
  .task-build/wabt/wasm-objdump "${flags[@]}" "$output.wasm" >"$output.actual"
  awk '/^\(;; STDOUT ;;;/{p=1;next} /^;;; STDOUT ;;\)/{p=0} p' "$input" >"$output.expected"
  test -s "$output.expected"
  cmp "$output.expected" "$output.actual"
  echo "UPSTREAM_OBJDUMP_CASE $name EXACT"
done
echo 'WABT_EXISTING_OBJECT_ROUNDTRIP cases=4'
