"use strict";
const assert=require('assert'),path=require('path'),{fork}=require('child_process');
async function main(){
 const {Corpus}=require(path.join(process.env.TASK_BUILD,'src/corpus.js'));
 const c=new Corpus([path.resolve('task_support/benign_seed.txt')],false); assert.strictEqual(c.getLength(),1);assert.strictEqual(c.generateInput().toString(),'begin');
 const child=fork(path.join(process.env.TASK_BUILD,'src/worker.js'),['task_support/benign_target.js'],{execArgv:[],stdio:['ignore','pipe','pipe','ipc']});
 const errors=[];child.stderr.on('data',b=>errors.push(b));
 let timer=setTimeout(()=>{child.kill();},10000);
 try{
  const values=[];
  for(const text of ['begin','advance','finish']){
   const m=await new Promise((resolve,reject)=>{
    const done=m=>{child.off('exit',exited);resolve(m);};
    const exited=(code,signal)=>{child.off('message',done);reject(new Error('worker exit '+code+' '+signal+' '+Buffer.concat(errors).toString()));};
    child.once('message',done);child.once('exit',exited);child.send({type:0,buf:Buffer.from(text)});
   });
   assert.strictEqual(m.type,0);assert(Number.isInteger(m.coverage)&&m.coverage>0);values.push(m.coverage);
  }
  assert(values[1]>=values[0]&&values[2]>=values[1]);
  console.log('BASELINE_WORKER_FEEDBACK '+values.join(',')+' corpus=1');
 }finally{clearTimeout(timer);await new Promise(resolve=>{if(child.exitCode!==null||child.signalCode!==null)return resolve();child.once('exit',resolve);child.kill();});}
}
main().then(()=>console.log('BASELINE_COMPLETE'),e=>{console.error(e);process.exitCode=1;});
