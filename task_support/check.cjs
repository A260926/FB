'use strict';
const test=require('node:test');
const assert=require('node:assert/strict');
const registered=new Set();
function checked(name,body){
 if(typeof name!=='string'||registered.has(name)||typeof body!=='function')throw new Error('invalid checked group');
 registered.add(name);
 return test(name,async()=>{
  let count=0;
  await body((actual,expected)=>{++count;assert.deepStrictEqual(actual,expected);});
  assert.ok(count>0,'group has no checks');
  console.log('TASK_GROUP '+name+' checks='+count+' COMPLETE');
 });
}
module.exports={checked};
