'use strict';
const fs=require('node:fs'),cp=require('node:child_process'),path=require('node:path'),os=require('node:os'),assert=require('node:assert/strict');
const root=path.resolve(__dirname,'..');
for(const file of ['extras/fairing.js','src/variational/surface-operator.js','src/variational/fairing-plan.js','task_tests/fairing.cjs','examples/fairing_obj.cjs'])assert.ok(fs.statSync(path.join(root,file)).isFile()&&fs.statSync(path.join(root,file)).size>0,'required nonempty file '+file);
function run(file,args=[]){const r=cp.spawnSync(process.execPath,[file,...args],{cwd:root,encoding:'utf8',timeout:180000,maxBuffer:8*1024*1024});if(r.stdout)process.stdout.write(r.stdout);if(r.stderr)process.stderr.write(r.stderr);assert.ok(!r.error&&r.status===0&&!r.signal,'child failed '+file);return r.stdout||'';}
const output=run('task_support/driver.cjs');
for(const g of ['metric','constraints','reuse'])assert.ok(new RegExp('^GROUP '+g+' cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$','m').test(output),'incomplete '+g);
assert.ok(/^TARGET_COMPLETE cases=[1-9][0-9]*$/m.test(output),'early exit');
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'nurbs-fairing-'));
try{
 const obj=path.join(dir,'fairing.obj');run('examples/fairing_obj.cjs',[obj]);assert.ok(fs.statSync(obj).size>0,'missing new output');
 const rows=fs.readFileSync(obj,'utf8').trim().split(/\r?\n/);let v=0,n=0,f=0;
 for(const line of rows){const p=line.trim().split(/\s+/);if(p[0]==='v'||p[0]==='vn'){assert.equal(p.length,4);assert.ok(p.slice(1).every(x=>Number.isFinite(Number(x))));if(p[0]==='v')v++;else{n++;assert.ok(Math.abs(Math.hypot(...p.slice(1).map(Number))-1)<1e-6);}}else if(p[0]==='f'){assert.equal(p.length,4);f++;}}
 assert.ok(v>0&&n>0&&f>0,'nonempty OBJ geometry required');console.log('OBJ_COMPLETE vertices='+v+' normals='+n+' triangles='+f);
}finally{fs.rmSync(dir,{recursive:true,force:true});}
run('task_support/baseline.cjs');console.log('FAIRING_TARGET_COMPLETE');
