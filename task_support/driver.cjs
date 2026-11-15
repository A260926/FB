'use strict';
const assert=require('node:assert/strict');
const cases=require('../task_tests/target.cjs');
const sampleCad=require('../extras/sample-cad');
const nurbs=require('../nurbs');
const groups=['jets','poles','topology'];
(async()=>{
 assert.equal(typeof sampleCad,'function');
 assert.ok(Array.isArray(cases)&&cases.length>0&&cases.length<10000);
 const names=new Set();
 for(const t of cases){assert.ok(t&&groups.includes(t.group)&&typeof t.name==='string'&&t.name&&typeof t.run==='function');assert.ok(!names.has(t.group+' '+t.name));names.add(t.group+' '+t.name);}
 let total=0;
 for(const group of groups){let ran=0,checks=0;
  for(const t of cases.filter(t=>t.group===group)){let n=0;
   const check=(value,message)=>{n++;assert.ok(value,message||'target assertion');};
   await t.run(check,{sampleCad,nurbs});assert.ok(n>0,'no checks in '+t.name);
   console.log(`CASE ${group} ${t.name} checks=${n}`);ran++;checks+=n;total++;
  }
  assert.ok(ran>0&&checks>0,'missing group '+group);
  console.log(`GROUP ${group} cases=${ran} checks=${checks} COMPLETE`);
 }
 assert.equal(total,cases.length);console.log(`TARGET_COMPLETE cases=${total}`);
})().catch(e=>{console.error(e.stack||e);process.exitCode=1;});
