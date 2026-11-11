#!/usr/bin/env bash
set -euo pipefail
test -s checks/closest-geometry.test.mjs
out=$(mktemp)
trap 'rm -f "$out"' EXIT
node --test --test-reporter=tap checks/closest-geometry.test.mjs | tee "$out"
grep -Eq '^# tests [1-9][0-9]*$' "$out"
grep -Eq '^# pass [1-9][0-9]*$' "$out"
grep -q '^# fail 0$' "$out"
grep -q '^# skipped 0$' "$out"
grep -Eq '^ok [0-9]+ - closest-geometry' "$out"
node node_modules/vitest/vitest.mjs run test/ShapeCasts.test.js -t 'Distance To Geometry'
echo CLOSEST_GEOMETRY_TARGET_OK
