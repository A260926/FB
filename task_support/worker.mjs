import assert from 'node:assert/strict';
import fs from 'node:fs';
const target='checks/rebind.target.mjs';assert.ok(fs.statSync(target).size>0,'nonempty target required');
const {cases}=await import('../'+target);
for(const group of ['transaction','shared_lifecycle','binding_shapes','skinning']){
 assert.equal(typeof cases?.[group],'function',group+' missing');let count=0;
 const result=cases[group]((condition,message)=>{assert.equal(typeof condition,'boolean','condition must be boolean');assert.ok(condition,message);++count;});
 assert.equal(result,undefined,'groups must be synchronous with no skip result');assert.ok(count>0,group+' made no checks');
 console.log(`GROUP ${group} checks=${count} COMPLETE`);
}
console.log('TARGET_COMPLETE groups=4');
