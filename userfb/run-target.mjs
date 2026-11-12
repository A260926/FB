// Public entry: fail on missing, empty, skipped or zero-matching target tests.
import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
const file='userfb/frame-geometry.test.mjs';
const titles=['global geometry and pixels','clipped pointer and offset','ancestor stability and abort','frame replacement and scroll'];
assert(existsSync(file),`Create ${file} with the four documented node:test cases first`);
function run(cmd,args,capture=false){
  const p=spawnSync(cmd,args,{encoding:'utf8',stdio:capture?'pipe':'inherit'});
  if(capture){process.stdout.write(p.stdout??'');process.stderr.write(p.stderr??'');}
  if(p.error)throw p.error;
  assert.equal(p.status,0,`${cmd} failed`);
  return p.stdout;
}
run('npm',['exec','--workspace','puppeteer-core','--','hereby','build']);
const tap=run(process.execPath,['--test','--test-reporter=tap',file],true);
for(const title of titles){
  const passed=tap.split('\n').some(line=>new RegExp(`^ok [0-9]+ - ${title}$`).test(line));
  assert(passed,`Required non-skipped top-level test did not pass: ${title}`);
}
assert(/^# pass [4-9][0-9]*$/m.test(tap)||/^# pass [1-9][0-9]{1,}$/m.test(tap),'at least four tests must run');
assert(/^# fail 0$/m.test(tap)&&/^# skipped 0$/m.test(tap)&&/^# todo 0$/m.test(tap));
run(process.execPath,['--test','--test-reporter=spec',
  'packages/puppeteer-core/lib/cjs/puppeteer/util/encoding.test.js',
  'packages/puppeteer-core/lib/cjs/puppeteer/common/EventEmitter.test.js',
  'packages/puppeteer-core/lib/cjs/puppeteer/cdp/NetworkManager.test.js']);
console.log('TARGET geometry-and-lifecycle PASS');
