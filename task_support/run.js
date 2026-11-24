"use strict";
const fs=require('fs'),path=require('path'),assert=require('assert'),cp=require('child_process');
async function main(){
 const mode=process.argv[2]||'baseline';
 if(mode==='baseline'){const p=cp.spawnSync(process.execPath,['task_support/baseline.js'],{stdio:'inherit',env:process.env,timeout:20000});assert.strictEqual(p.status,0,'production baseline failed');return;}
 assert.strictEqual(mode,'target');
 const p=cp.spawnSync(process.execPath,['task_support/api_consumer.js'],{stdio:'inherit',env:process.env,timeout:20000});assert.strictEqual(p.status,0,'fixed API consumer failed');
 const outDir=fs.mkdtempSync(path.resolve('.task-build/example-output.'));const transcript=path.join(outDir,'transcript.txt');
 const e=cp.spawnSync(process.execPath,['examples/replay.js',transcript],{stdio:'inherit',env:process.env,timeout:25000});assert.strictEqual(e.status,0,'required example failed');assert(fs.statSync(transcript).size>0,'required fresh output missing');assert(fs.readFileSync(transcript,'utf8').includes('stateful_replay_ok'));
 const tests=require(path.resolve('checks/replay.test.js'));
 for(const name of ['stages','priming','protocol']){
  assert.strictEqual(typeof tests[name],'function','missing group '+name);let checks=0,failed=0;
  const c={root:process.cwd(),production:name=>require(path.join(process.env.TASK_BUILD,'src',name+'.js')),check(ok,message){checks++;if(!ok){failed++;throw new Error(message||'check failed');}}};
  await tests[name](c);assert(checks>0,'zero content checks '+name);assert.strictEqual(failed,0);console.log('GROUP '+name+' checks='+checks+' COMPLETE');
 }
 console.log('TARGET_COMPLETE');
}
main().catch(e=>{console.error(e);process.exitCode=1;});
