import {test} from 'node:test';
import assert from 'node:assert/strict';
export function taskCase(name, fn) {
 test(name, async()=>{
  let n=0;
  await fn((condition, message)=>{assert.ok(condition,message); n++;});
  assert.ok(n>0,'case must perform content checks');
  console.log(`TASK_DONE ${name} assertions ${n}`);
 });
}
