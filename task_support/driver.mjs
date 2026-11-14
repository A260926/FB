import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const { cases } = await import(pathToFileURL(resolve('tests/portal-collector.test.mjs')).href);
assert.ok(Array.isArray(cases) && cases.length > 0 && cases.length < 10000, 'nonempty registered cases');
const groups = ['geometry', 'membership', 'integration'];
assert.ok(cases.every(c => c && groups.includes(c.group) && typeof c.name === 'string' && c.name.length > 0 && typeof c.run === 'function' && !c.skip), 'valid, non-skipped cases');
let total = 0;
for (const group of groups) {
 let ran = 0, checks = 0;
 for (const test of cases.filter(c => c.group === group)) {
  let count = 0, failed = false;
  const check = (value, message = 'content assertion') => { count++; if (!value) failed = true; assert.ok(value, message); };
  await test.run(check);
  assert.ok(count > 0 && !failed, `${test.name}: no content assertions`);
  console.log(`PORTAL_CASE ${group} ${test.name} checks=${count}`); ran++; checks += count; total++;
 }
 assert.ok(ran > 0 && checks > 0, `${group}: no executed cases`);
 console.log(`PORTAL_GROUP ${group} cases=${ran} checks=${checks} COMPLETE`);
}
assert.equal(total, cases.length);
console.log(`PORTAL_TARGET_COMPLETE cases=${total}`);
