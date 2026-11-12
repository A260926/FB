import fs from 'node:fs';
import assert from 'node:assert/strict';
const s = fs.readFileSync('.build/stream-normal.tap', 'utf8');
const n = key => {
 const a = [...s.matchAll(new RegExp('^# ' + key + ' (\\d+)$', 'gm'))];
 assert.equal(a.length, 1, 'missing or ambiguous normal test summary: ' + key);
 return Number(a[0][1]);
};
assert.ok(n('tests') >= 1 && n('pass') >= 1, 'no actual normal tests passed');
for (const k of ['fail', 'cancelled', 'skipped', 'todo']) assert.equal(n(k), 0, k);
assert.match(s, /^ok \d+ - stream acquisition contract$/m);
