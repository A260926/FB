import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
const suite = await import(pathToFileURL(process.cwd()+'/switch_recovery_test.mjs'));
assert.ok(Array.isArray(suite.cases)&&suite.cases.length>0&&suite.cases.length<10000,'nonempty cases export required');
const groups=['aliases','fallthrough','exits'];let total=0;
for(const group of groups){let ran=0,n=0;
 for(const c of suite.cases){assert.ok(c&&typeof c.name==='string'&&typeof c.run==='function'&&groups.includes(c.group));if(c.group!==group)continue;
 let checks=0;const check=(value,message)=>{checks++;assert.ok(value,message);};
 await c.run(check);assert.ok(checks>0,'case must execute checks');ran++;n+=checks;total++;console.log(`CASE ${group} ${c.name} checks=${checks}`);
 }
 assert.ok(ran>0&&n>0,'group must execute checks');console.log(`GROUP ${group} cases=${ran} checks=${n} COMPLETE`);
}
assert.equal(total,suite.cases.length);console.log(`TARGET_COMPLETE cases=${total}`);
