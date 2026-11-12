#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cc -std=c99 -O1 -I. mpc.c baseline/smoke.c -lm -o "$build/smoke"
"$build/smoke"
cc -std=c99 -O1 -I. mpc.c baseline/semantic-witness.c -lm -o "$build/semantics"
"$build/semantics"
cc -std=c99 -O1 -I. mpc.c tests/test.c tests/ptest.c tests/core.c tests/regex.c tests/grammar.c tests/combinators.c -lm -o "$build/old"
"$build/old" > "$build/old.log"
cat "$build/old.log"
sed -E $'s/\033\\[[0-9;]*m//g' "$build/old.log" > "$build/old.plain"
grep -Eq '^  \| Suites  \|\| Total +4 +\| Passed +4 +\| Failed +0 +\|' "$build/old.plain"
grep -Eq '^  \| Tests   \|\| Total +30 +\| Passed +30 +\| Failed +0 +\|' "$build/old.plain"
grep -Eq '^  \| Asserts \|\| Total +142 +\| Passed +142 +\| Failed +0 +\|' "$build/old.plain"
grep -Eq '^      Total Running Time:' "$build/old.plain"
echo 'MPC_OLD_TESTS_OK suites=4 tests=30 asserts=142'
