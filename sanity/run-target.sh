#!/usr/bin/env bash
set -euo pipefail
test -s sanity/tangent-frame.test.mjs
out=$(mktemp)
trap 'rm -f "$out"' EXIT
node --test --test-reporter=tap sanity/tangent-frame.test.mjs | tee "$out"
grep -Eq '^# tests [1-9][0-9]*$' "$out"
grep -Eq '^# pass [1-9][0-9]*$' "$out"
grep -q '^# fail 0$' "$out"
grep -q '^# skipped 0$' "$out"
grep -Eq '^ok [0-9]+ - tangent-frame' "$out"
node node_modules/vitest/vitest.mjs run tests/CSG.Evaluator.test.js
echo TANGENT_FRAME_TARGET_OK
