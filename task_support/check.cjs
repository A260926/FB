"use strict";
const assert = require('node:assert/strict');
const registered = [];
function group(name, fn) {
  if (typeof name !== 'string' || typeof fn !== 'function') throw new TypeError('group requires name and function');
  registered.push({name, fn, skip:false});
}
group.skip = (name, fn) => registered.push({name, fn, skip:true});
async function execute(expected) {
  assert.deepEqual(registered.map(g=>g.name).sort(), [...expected].sort(), 'required groups must each be registered exactly once');
  const report=[];
  for (const g of registered) {
    assert.equal(g.skip, false, `SKIP forbidden: ${g.name}`);
    let assertions=0, failure=null;
    const c=Object.freeze({check(condition, message='content check failed') {
      assertions++;
      if (!condition) { failure ||= new assert.AssertionError({message}); throw failure; }
    }});
    await g.fn(c);
    if (failure) throw failure;
    assert.ok(assertions > 0, `no content assertions: ${g.name}`);
    report.push({name:g.name, assertions, status:'pass'});
    console.log(`GROUP_OK ${g.name} assertions=${assertions}`);
  }
  return report;
}
module.exports=Object.freeze({group,execute});
