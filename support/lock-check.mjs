import {createHash} from 'node:crypto';
import {readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import assert from 'node:assert/strict';

const files = ['package.json', 'package-lock.json'];
const hashes = Object.fromEntries(files.map(file => [file, createHash('sha256').update(readFileSync(file)).digest('hex')]));
mkdirSync('.cache', {recursive: true});
if (process.argv[2] === 'save') {
  writeFileSync('.cache/dependency-hashes.json', JSON.stringify(hashes));
} else {
  assert.deepEqual(hashes, JSON.parse(readFileSync('.cache/dependency-hashes.json', 'utf8')));
  const lock = JSON.parse(readFileSync('package-lock.json', 'utf8'));
  for (const [path, item] of Object.entries(lock.packages)) {
    if (path && !item.link) {
      assert.ok(item.version && item.integrity && item.resolved, path);
      assert.match(item.resolved, /^https:\/\/registry\.npmjs\.org\//, path);
    }
  }
  console.log(`LOCK_OK ${Object.keys(lock.packages).length - 1} resolved entries`);
}
