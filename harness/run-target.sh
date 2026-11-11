#!/usr/bin/env bash
set -euo pipefail
test -s harness/projective.test.cjs
out=$(mktemp)
trap 'rm -f "$out"' EXIT
node --test --test-reporter=tap harness/projective.test.cjs | tee "$out"
grep -Eq '^# tests [1-9][0-9]*$' "$out"
grep -Eq '^# pass [1-9][0-9]*$' "$out"
grep -q '^# fail 0$' "$out"
grep -q '^# skipped 0$' "$out"
grep -Eq '^ok [0-9]+ - projective' "$out"
test -s harness/projective-surface.cjs
mesh=$(mktemp)
node harness/projective-surface.cjs "$mesh"
node - "$mesh" <<'JS'
const fs=require('node:fs'),assert=require('node:assert/strict');
const lines=fs.readFileSync(process.argv[2],'utf8').trim().split(/\n/);
for(const tag of ['v ','vn ','f '])assert.ok(lines.filter(l=>l.startsWith(tag)).length>0);
for(const line of lines.filter(l=>l.startsWith('v ')||l.startsWith('vn ')))assert.ok(line.split(/\s+/).slice(1).every(x=>Number.isFinite(Number(x))));
for(const line of lines.filter(l=>l.startsWith('vn ')))assert.ok(Math.abs(Math.hypot(...line.split(/\s+/).slice(1).map(Number))-1)<1e-6);
console.log('PROJECTIVE_OBJ_OK');
JS
rm -f "$mesh"
node test/derivative.js
echo NURBS_PROJECTIVE_TARGET_OK
