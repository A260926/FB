import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { statSync } from 'node:fs';
import { randomBytes } from 'node:crypto';
import { fileURLToPath } from 'node:url';
process.chdir(fileURLToPath(new URL('..', import.meta.url)));
const target = "checks/hierarchy-batch.target.mjs";
assert.ok(statSync(target).size > 0, 'nonempty target file required');
for (const group of ["finalGraph", "observers", "activation", "failure"]) {
  const token = randomBytes(16).toString('hex');
  const child = spawnSync(process.execPath, ['task_support/driver.mjs', target, group, token], { encoding:'utf8', timeout:30000, maxBuffer:1024*1024 });
  process.stdout.write(child.stdout || '');
  process.stderr.write(child.stderr || '');
  assert.equal(child.error, undefined, `group process ${group}`);
  assert.equal(child.status, 0, `group status ${group}`);
  const lines = child.stdout.split(/\r?\n/);
  assert.ok(lines.some(line => new RegExp(`^TASK_GROUP_COMPLETE ${token} ${group} checks=[1-9][0-9]*$`).test(line)), `incomplete group ${group}`);
}
const baseline = spawnSync(process.execPath, ['checks/baseline.mjs'], { encoding:'utf8', timeout:30000 });
process.stdout.write(baseline.stdout || '');process.stderr.write(baseline.stderr || '');
assert.equal(baseline.status, 0, 'existing production baseline');
console.log('HIERARCHY_BATCH_TARGET_COMPLETE groups=4');
