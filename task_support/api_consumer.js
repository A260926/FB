"use strict";
const assert=require('assert'),path=require('path'),fs=require('fs'),crypto=require('crypto');
async function main(){
 const {ReplayController}=require(path.join(process.env.TASK_BUILD,'src/replay.js'));
 assert(ReplayController&&typeof ReplayController.create==='function','missing public ReplayController');
 const target=path.resolve('task_support/benign_target.js');
 let live=await ReplayController.create(target,[[Buffer.from('begin')],[Buffer.from('advance')]],'normal-build');
 let restored;
 try{
  assert.strictEqual(await live.step(),true);
  let state=live.state();assert.strictEqual(state.executions,1);assert.strictEqual(state.stage,1);assert.strictEqual(state.cursor,0);assert(state.coverage.length>0);
  const digest=crypto.createHash('sha256').update(fs.readFileSync(target)).digest('hex');
  const features=state.coverage.map(x=>JSON.parse(x));assert(features.some(x=>x[0]==='benign_target.js'&&x[1]===digest&&['s','f','b'].includes(x[2])),'coverage must bind actual target');
  const blob=await live.checkpoint();assert(Buffer.isBuffer(blob)&&blob.length>0);
  await live.close();live=null;
  restored=await ReplayController.restore(target,blob,'normal-build');
  assert.strictEqual(restored.state().executions,1);
  assert.strictEqual(await restored.step(),true);assert.strictEqual(restored.state().executions,2);
  assert.strictEqual(await restored.step(),false);
  console.log('FIXED_API_CONSUMER_COMPLETE');
 }finally{if(live)await live.close();if(restored)await restored.close();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
