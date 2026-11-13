import assert from 'node:assert/strict';
const {default:cases}=await import('../test/scene/local-affine-target.mjs');
assert.ok(Array.isArray(cases)&&cases.length>=5,'five cases required');
const names=new Set();let assertions=0;
for(const c of cases){assert.ok(c&&typeof c.name==='string'&&!names.has(c.name)&&typeof c.run==='function');names.add(c.name);let count=0;await c.run((condition,message)=>{count++;assertions++;assert.ok(condition,message);});assert.ok(count>0,'no checks in '+c.name);console.log('affine case '+c.name+' ok');}
console.log('LOCAL_AFFINE_OK cases='+cases.length+' assertions='+assertions);
