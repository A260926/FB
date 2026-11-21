'use strict';
const assert = require('node:assert/strict');
require('../src/variational/surface-operator');
require('../src/variational/fairing-plan');
const api={nurbs:require('../nurbs'),createFairingPlan:require('../extras/fairing')};
assert.equal(typeof api.createFairingPlan,'function');
const cases=require('../task_tests/fairing.cjs');
async function main(){
 assert.ok(Array.isArray(cases)&&cases.length>0&&cases.length<=10000,'nonempty case array required');
 const groups=['metric','constraints','reuse'];let total=0;
 for(const g of groups){let ran=0,checks=0;
  for(const c of cases){assert.ok(c&&typeof c.group==='string'&&typeof c.name==='string'&&c.name.length&&typeof c.run==='function');if(c.group!==g)continue;
   let n=0;const check=(value,message)=>{n++;assert.ok(value,message||c.name);};
   const result=await c.run(check,api);assert.ok(result===undefined||result===0,'case returned failure/skip');assert.ok(n>0,'case made no assertions');
   checks+=n;ran++;total++;console.log('CASE '+g+' '+c.name+' checks='+n);
  }
  assert.ok(ran>0&&checks>0,'required group absent');console.log('GROUP '+g+' cases='+ran+' checks='+checks+' COMPLETE');
 }
 assert.equal(total,cases.length,'unknown case group');console.log('TARGET_COMPLETE cases='+total);
}
main().catch(e=>{console.error(e.stack||e);process.exitCode=1;});
