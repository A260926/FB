import assert from 'node:assert/strict';
const {default:cases}=await import('../test/skin-world.mjs');
assert.ok(Array.isArray(cases)&&cases.length>=5,'at least five named cases');
const names=new Set();let completed=0;
for(const c of cases){
  assert.ok(c&&typeof c.name==='string'&&c.name.trim()&&typeof c.run==='function');
  assert.ok(!names.has(c.name),'duplicate case');names.add(c.name);
  let checks=0;
  const result=await c.run((condition,message)=>{++checks;assert.ok(condition,message);});
  assert.ok(checks>0,'case without content assertions');
  assert.equal(result,undefined,'case must finish normally without skip/status returns');
  console.log('CASE '+c.name+' checks='+checks);++completed;
}
assert.equal(completed,cases.length);
console.log('SKIN_WORLD_COMPLETE cases='+completed);
await import('./normal.mjs');
