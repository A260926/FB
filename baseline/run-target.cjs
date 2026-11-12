'use strict';
const fs = require('node:fs');
const { spawnSync } = require('node:child_process');
const path = require('node:path');
function fail(message) { console.error(message); process.exit(1); }
const file = 'test/harness/regexp-flow.test.cjs';
if (!fs.existsSync(file)) fail(`Missing required new test file: ${file}`);
if (!fs.existsSync('docs/regexp-flow.md') || !fs.readFileSync('docs/regexp-flow.md', 'utf8').trim()) fail('Missing required new documentation: docs/regexp-flow.md');
function run(executable, args, timeout) {
  const result = spawnSync(executable, args, { encoding: 'utf8', timeout, maxBuffer: 8 * 1024 * 1024 });
  process.stdout.write(result.stdout || '');
  process.stderr.write(result.stderr || '');
  if (result.error || result.signal || result.status !== 0) fail(`Command failed: ${result.error?.message || result.signal || result.status}`);
  return result.stdout;
}
const tap = run(process.execPath, ['--test', '--test-reporter=tap', file], 60000);
for (const name of ['program-points', 'joins-and-exits', 'loop-fixpoint', 'rule-isolation']) {
  if (!new RegExp(`^ok \\d+ - ${name}\\r?$`, 'm').test(tap)) fail(`Required top-level test did not pass: ${name}`);
}
function count(label) {
  const matches = [...tap.matchAll(new RegExp(`^# ${label} (\\d+)\\r?$`, 'gm'))];
  if (matches.length !== 1) fail(`Missing or ambiguous TAP ${label} summary`);
  return Number(matches[0][1]);
}
const total = count('tests');
if (total < 4 || count('pass') !== total || ['fail', 'cancelled', 'skipped', 'todo'].some(k => count(k) !== 0)) {
  fail('New tests must execute, all pass, and have no skipped/todo/cancelled cases');
}
const old = run(process.execPath, [path.join('baseline', 'node_modules', 'mocha', 'bin', 'mocha.js'),
  'test/rules/detect-unsafe-regexp.js', 'test/rules/detect-non-literal-regexp.js',
  'test/utils/is-static-expression.js', '--reporter', 'dot'], 30000);
if (!/^\s*40 passing\b/m.test(old) || /\bpending\b/.test(old)) fail('Expected all 40 selected upstream tests');
console.log(`TARGET_FLOW new=${total} old=40 all passed`);
