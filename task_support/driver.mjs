import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import { resolve } from 'node:path';
const [target, group, token] = process.argv.slice(2);
const { cases } = await import(pathToFileURL(resolve(target)).href);
assert.ok(cases && typeof cases === 'object' && !Array.isArray(cases), 'registered cases object required');
const run = cases[group];
assert.equal(typeof run, 'function', `missing group ${group}`);
assert.ok(!run.skip, `skipped group ${group}`);
let checks = 0, failed = false;
const check = (value, message = 'content assertion') => {
  checks++;
  if (!value) failed = true;
  assert.ok(value, message);
};
const result = run(check);
assert.ok(!result || typeof result.then !== 'function', 'synchronous cases required');
assert.ok(checks > 0 && !failed, 'group must execute successful content assertions');
console.log(`TASK_GROUP_COMPLETE ${token} ${group} checks=${checks}`);
