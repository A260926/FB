import assert from 'node:assert/strict';
export const cases=[];
export function taskCase(name,run){assert.equal(typeof run,'function');cases.push({name,run});}
export async function execute(){
 assert.equal(cases.length,3,'exactly three groups');
 assert.deepEqual([...cases.map(c=>c.name)].sort(),['compositing','geometry','timeline']);
 for(const c of cases){let count=0;let open=true;
  const check=(condition,message='check failed')=>{assert.ok(open,'late check');count++;assert.ok(condition,message);};
  await c.run(check);open=false;assert.ok(count>0,'no assertions: '+c.name);
  console.log('TASK_GROUP_DONE '+c.name+' checks='+count);
 }
 console.log('DRAWING_TARGET_DONE');
}
