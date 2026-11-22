'use strict';
const assert=require('node:assert/strict'), path=require('node:path');
const cases=require(path.resolve('tests/pts-playback.cjs'));
assert.ok(Array.isArray(cases)&&cases.length>0&&cases.length<=10000);
let total=0;
for(const group of ['transport','anchors','playout']) {
  let ran=0,checks=0;const names=new Set();
  for(const c of cases) {
    assert.ok(c&&typeof c.name==='string'&&c.name&&typeof c.run==='function');
    if(c.group!==group) continue;
    assert.ok(!names.has(c.name));names.add(c.name);let count=0;
    const result=c.run((v,message)=>{count++;assert.ok(v,message);});
    assert.equal(result,undefined,'run must complete synchronously without returning skip/status');
    assert.ok(count>0,'case executed no checks');
    ran++;checks+=count;total++;
  }
  assert.ok(ran>0&&checks>0);
  console.log('GROUP '+group+' cases='+ran+' checks='+checks+' COMPLETE');
}
assert.equal(total,cases.length);
console.log('TARGET_COMPLETE cases='+total);
