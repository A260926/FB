#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s test/private-scope-model.test.mjs
log=$(mktemp)
trap 'rm -f "$log"' EXIT
node --test --test-reporter=tap --test-concurrency=1 test/private-scope-model.test.mjs | tee "$log"
for name in forward nested heritage accessors model mutants; do
 grep -Eq "^ok [0-9]+ - ${name}$" "$log"
done
grep -Eq '^# tests [1-9][0-9]*$' "$log"
echo ACORN_PRIVATE_SCOPE_MODEL_OK
bash baseline/run.sh
