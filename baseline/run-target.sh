#!/usr/bin/env bash
# Acceptance gate for the model-driven private-name syntax regression.
#
# 1. Runs the six named node:test cases in test/private-scope-model.test.mjs
#    and verifies that each of them actually ran and passed.
# 2. Re-runs six existing private-name parse checks from the repository's
#    own test suite against a freshly loaded copy of acorn/src.
set -euo pipefail
cd "$(dirname "$0")/.."

node -e 'const major = Number(process.versions.node.split(".")[0]); if (!(major >= 22)) { console.error(`Node >= 22 required, got ${process.versions.node}`); process.exit(1) }'

echo "== private scope model: six named tests =="
if ! tap="$(node --test --test-reporter=tap test/private-scope-model.test.mjs 2>&1)"; then
  printf '%s\n' "$tap"
  echo "private-scope-model tests failed" >&2
  exit 1
fi
printf '%s\n' "$tap"
for name in forward nested heritage accessors model mutants; do
  if ! grep -Eq "^ok [0-9]+ - ${name}$" <<< "$tap"; then
    echo "named test missing or not PASS: ${name}" >&2
    exit 1
  fi
done
if grep -Eq "^not ok" <<< "$tap"; then
  echo "unexpected failing test in private-scope-model run" >&2
  exit 1
fi
echo "ACORN_PRIVATE_SCOPE_MODEL_OK"

echo "== private name baseline: six existing parse checks =="
node baseline/run-private-baseline.mjs
