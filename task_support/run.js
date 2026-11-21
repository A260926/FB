'use strict';
const fs=require('node:fs');
const os=require('node:os');
const path=require('node:path');
const cp=require('node:child_process');
const root=path.resolve(__dirname,'..');
process.chdir(root);
function run(file,args=[]) {
 const p=cp.spawnSync(process.execPath,[file,...args],{cwd:root,encoding:'utf8',timeout:180000,maxBuffer:8*1024*1024});
 if(p.stdout)process.stdout.write(p.stdout);
 if(p.stderr)process.stderr.write(p.stderr);
 if(p.error)throw p.error;
 if(p.status!==0)throw Error(`${file} failed status=${p.status} signal=${p.signal}`);
 return p.stdout;
}
try {
 const mode=process.argv[2]||'baseline';
 if(mode==='baseline')run('task_support/baseline.js');
 else if(mode==='target') {
  for(const file of ['terrain-clipmap.js','test/clipmap.js','examples/clipmap.js'])
   if(!fs.statSync(file).size)throw Error(`empty ${file}`);
  const output=run('task_support/checks.js');
  for(const group of ['windows','updates','render'])
   if(!new RegExp(`^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$`,'m').test(output))throw Error(`incomplete ${group}`);
  if(!/^TARGET_COMPLETE cases=3$/m.test(output))throw Error('test process exited before completion');
  const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'voxelspace-example-'));
  try {
   const image=path.join(tmp,'frame.ppm');
   run('examples/clipmap.js',[image]);
   const data=fs.readFileSync(image);
   if(data.length<12||!/^P[36]\s/.test(data.toString('ascii',0,3)))throw Error('missing fresh PPM');
   console.log('EXAMPLE_COMPLETE fresh_ppm=1');
  } finally {fs.rmSync(tmp,{recursive:true,force:true});}
 } else throw Error(`unknown mode ${mode}`);
} catch(e) {console.error(e.stack||e);process.exitCode=1;}
