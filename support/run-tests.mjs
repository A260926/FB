import {existsSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
const [file, ...expected] = process.argv.slice(2);
if (!file || !existsSync(file) || expected.length === 0 || new Set(expected).size !== expected.length) {
  console.error('Missing test file or invalid required test names');
  process.exit(1);
}
const result = spawnSync(process.execPath, [
  '--import=tsx/esm', '--test', '--test-timeout=15000',
  '--test-reporter=./support/reporter.mjs', file,
], {
  stdio: 'inherit',
  env: {...process.env, CHECK_EXPECTED_TESTS: JSON.stringify(expected)},
});
if (result.error) console.error(result.error.message);
process.exit(result.status ?? 1);
