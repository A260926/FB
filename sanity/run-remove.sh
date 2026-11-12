#!/usr/bin/env bash
set -euo pipefail
test -s test/remove.test.js
out=$(mktemp)
trap 'rm -f "$out"' EXIT
node --test --test-reporter=tap test/remove.test.js | tee "$out"
grep -Eq '^# pass [1-9][0-9]*$' "$out"
grep -Eq '^# fail 0$' "$out"
grep -Eq '^# skipped 0$' "$out"
bash sanity/baseline.sh
echo 'DELAUNATOR_REMOVE_PASS'
