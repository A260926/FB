'use strict';
const fs=require('node:fs');const cp=require('node:child_process');const path=require('node:path');const assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');process.chdir(root);
function run(file,args=[]){const p=cp.spawnSync(process.execPath,[file,...args],{cwd:root,encoding:'utf8',timeout:180000,env:process.env,maxBuffer:8*1024*1024});if(p.stdout)process.stdout.write(p.stdout);if(p.stderr)process.stderr.write(p.stderr);if(p.error)throw p.error;assert.equal(p.signal,null);assert.equal(p.status,0,'failed '+file);return p.stdout;}
const mode=process.argv[2]||'baseline';assert.ok(['target','baseline'].includes(mode));
if(mode==='target'){
 for(const p of ['task_tests/target.cjs','extras/sample-cad.js','examples/cad-poles.cjs'])assert.ok(fs.statSync(p).size>0,'missing or empty '+p);
 const output=run('task_support/driver.cjs');
 for(const g of ['jets','poles','topology'])assert.ok(new RegExp('^GROUP '+g+' cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$','m').test(output),'incomplete '+g);
 assert.ok(/^TARGET_COMPLETE cases=[1-9][0-9]*$/m.test(output),'early process exit');
 fs.mkdirSync('.task-build',{recursive:true});const dir=fs.mkdtempSync(path.resolve('.task-build/cad-output-'));
 run('examples/cad-poles.cjs',[dir]);
 run('task_support/check-obj.cjs',[path.join(dir,'roundtop.obj'),path.join(dir,'cone.obj')]);
}
run('task_support/baseline.cjs');
