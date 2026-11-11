#!/usr/bin/env bash
set -euo pipefail
log=$(mktemp)
trap 'rm -f "$log"' EXIT
NODE_PATH="$PWD/author/node_modules" node --require ./author/node_modules/coffee-script/register --test --test-reporter=tap tests/frame_elements.test.cjs | tee "$log"
grep -E '^ok [0-9]+ - frame-element-contract$' "$log" > /dev/null
grep -E '^# tests [1-9][0-9]*$' "$log" > /dev/null
