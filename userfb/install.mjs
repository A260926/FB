// Reproducible author/solver setup, no task implementation. Apache-2.0.
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdirSync} from 'node:fs';
assert.equal(process.versions.node.split('.')[0], '22', 'Use Node.js 22');
mkdirSync('.userfb-build',{recursive:true});
const env={...process.env,PUPPETEER_SKIP_DOWNLOAD:'true',npm_config_cache:`${process.cwd()}/.userfb-build/npm-cache`};
for(const [cmd,args] of [
  ['node',['--version']],['npm',['--version']],
  ['npm',['ci','--ignore-scripts']],
  ['npm',['run','build','--workspace','@puppeteer/browsers']],
  ['npm',['exec','--workspace','puppeteer-core','--','hereby','build']],
  ['docker',['pull','chromedp/headless-shell@sha256:d0c5b37a80f6180f28b2824d8844bf0c10bdf54ebaaf1f97514b66e5a20bef4f']],
]) {
  console.log('SETUP',cmd,...args);
  const p=spawnSync(cmd,args,{env,stdio:'inherit'});
  if(p.error) throw p.error;
  assert.equal(p.status,0,`${cmd} failed`);
}
console.log('SETUP locked-dependencies PASS');
