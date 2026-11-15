import assert from 'node:assert/strict';
const {cases}=await import('../tests/scene-instance-batches.test.mjs');
assert(Array.isArray(cases)&&cases.length>0&&cases.length<=10000,'nonempty cases array required');
const groups=['membership','submission','integration'];let total=0;
for(const group of groups){
 let ran=0, groupChecks=0;
 for(const item of cases){
  assert(item && groups.includes(item.group) && typeof item.name==='string' && item.name.length && typeof item.run==='function' && !item.skip && !item.todo,'invalid or skipped case');
  if(item.group!==group)continue;
  let checks=0,failed=0;
  const check=(value,message='content check')=>{checks++;if(!value){failed++;throw new Error(message);}};
  await item.run(check);
  assert(checks>0&&failed===0,'case must finish with successful content checks');
  console.log('CASE '+group+' '+item.name+' checks='+checks);ran++;groupChecks+=checks;total++;
 }
 assert(ran>0&&groupChecks>0,'missing group '+group);
 console.log('GROUP '+group+' cases='+ran+' checks='+groupChecks+' COMPLETE');
}
assert.equal(total,cases.length);console.log('TARGET_COMPLETE cases='+total);
