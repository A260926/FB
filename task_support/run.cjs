"use strict";
const fs=require('node:fs'), path=require('node:path'), assert=require('node:assert/strict');
const {spawnSync}=require('node:child_process');
const config=require('./target.json');
process.chdir(path.resolve(__dirname,'..'));
fs.mkdirSync('.task-build',{recursive:true});
function run(args, timeout=45000) {
 const r=spawnSync(process.execPath,args,{encoding:'utf8',timeout,maxBuffer:4*1024*1024,env:{...process.env,NODE_PATH:'',NODE_OPTIONS:''}});
 if(r.stdout) process.stdout.write(r.stdout);
 if(r.stderr) process.stderr.write(r.stderr);
 if(r.error) throw r.error;
 assert.equal(r.status,0,`child failed: ${args.join(' ')}`);
}
if(config.build)run([config.build]);
if((process.argv[2]||'target')==='baseline') {
 for(const args of config.baseline)run(args);
 console.log('BASELINE_OK');
} else if(process.argv[2]==='target') {
 assert.ok(fs.existsSync(config.target),'required target file missing: '+config.target);
 assert.ok(fs.statSync(config.target).size>0,'target is empty');
 fs.rmSync('.task-build/report.json',{force:true});
 run(['task_support/execute.cjs']);
 assert.ok(fs.existsSync('.task-build/report.json'),'target exited without complete report');
 const report=JSON.parse(fs.readFileSync('.task-build/report.json','utf8'));
 assert.deepEqual(report.map(g=>g.name).sort(),[...config.groups].sort());
 assert.ok(report.every(g=>g.status==='pass'&&g.assertions>0),'all required groups must assert and pass');
 console.log('TARGET_WRAPPER_OK');
} else throw new Error('usage: bash task_support/run.sh baseline|target');
