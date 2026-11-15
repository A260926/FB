'use strict';
const fs=require('node:fs');const assert=require('node:assert/strict');
for(const file of process.argv.slice(2)){
 const pos=[],uv=[],norm=[],faces=[];
 for(const line of fs.readFileSync(file,'utf8').split(/\r?\n/)){
  const [tag,...v]=line.trim().split(/\s+/);if(!['v','vt','vn','f'].includes(tag))continue;
  if(tag==='f'){assert.equal(v.length,3);faces.push(v);continue;}
  const p=v.map(Number);assert.equal(p.length,tag==='vt'?2:3);assert.ok(p.every(Number.isFinite));
  if(tag==='v')pos.push(p);else if(tag==='vt')uv.push(p);else{assert.ok(Math.abs(Math.hypot(...p)-1)<1e-7);norm.push(p);}
 }
 assert.ok(pos.length>=3&&uv.length>=3&&norm.length>=3&&faces.length>0);
 for(const f of faces){const refs=f.map(t=>{const a=t.split('/').map(Number);assert.equal(a.length,3);for(let j=0;j<3;j++)assert.ok(Number.isInteger(a[j])&&a[j]>0&&a[j]<=[pos,uv,norm][j].length);return a[0]-1;});assert.equal(new Set(refs).size,3);
  const [a,b,c]=refs.map(i=>pos[i]);const x=b.map((v,i)=>v-a[i]),y=c.map((v,i)=>v-a[i]);assert.ok(Math.hypot(x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0])>1e-14);
 }
 console.log(`CAD_OBJ_OK vertices=${pos.length} triangles=${faces.length}`);
}
