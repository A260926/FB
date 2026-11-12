#!/bin/sh
set -eu
for f in checks/rank-contract.test.mjs checks/run-rank-mutants.mjs; do test -s "$f" || { echo "missing target: $f" >&2; exit 1; }; done
npm run build
log=$(mktemp)
trap 'rm "$log"' EXIT
node --test --test-reporter=tap checks/rank-contract.test.mjs >"$log" 2>&1 || { cat "$log"; exit 1; }
cat "$log"
grep -Eq '^# pass [1-9][0-9]*$' "$log"
if grep -Eq '^# (skipped|todo) [1-9]' "$log"; then echo "target tests contain skips or todo" >&2; exit 1; fi
grep -Eq '^ok [0-9]+ - rank-model$' "$log"
grep -Eq '^ok [0-9]+ - rank-contexts$' "$log"
node checks/run-rank-mutants.mjs >"$log" 2>&1 || { cat "$log"; exit 1; }
cat "$log"
for marker in CANDIDATE_GUARD_KILLED REVERSE_RANK_KILLED NON_ELEMENT_KILLED; do grep -Fx "$marker" "$log" >/dev/null; done
npm exec -- vitest run test/pseudo-classes.ts test/api.ts
