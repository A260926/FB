'use strict';
const path=require('node:path');
const fs=require('node:fs');
const root=path.resolve(__dirname,'..');
for(const file of ['terrain-clipmap.js','test/clipmap.js']) {
 const p=path.join(root,file);if(!fs.statSync(p).size)throw Error(`empty ${file}`);
}
const production=require(path.join(root,'terrain-clipmap.js'));
if(typeof production.TerrainSource!=='function'||typeof production.TerrainClipmap!=='function')throw Error('missing production exports');
const suite=require(path.join(root,'test/clipmap.js'));
const api={...production,...require('./viewer')};
let total=0;
for(const name of ['windows','updates','render']) {
 if(typeof suite[name]!=='function')throw Error(`missing group ${name}`);
 let checks=0,failed=false;
 const check=(condition,message)=>{
  checks++;
  if(!condition){failed=true;throw Error(message||`failed ${name}`);}
  return true;
 };
 const result=suite[name](api,check);
 if(failed||!Number.isSafeInteger(result)||result<1||result!==checks)throw Error(`no counted checks or wrong count: ${name}`);
 console.log(`GROUP ${name} cases=1 checks=${checks} COMPLETE`);
 total++;
}
console.log(`TARGET_COMPLETE cases=${total}`);
