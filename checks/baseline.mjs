import assert from 'node:assert/strict';
import { extrudePolygon } from '../src/main.js';
const polygon=[[[0,0],[4,0],[4,2],[0,2]],[[1,0.5],[1,1.5],[2,1.5],[2,0.5]]];
const before=JSON.stringify(polygon);
const mesh=extrudePolygon([polygon],{depth:4,bevelSize:0,smoothSide:false});
let volume=0;const levels=new Set();
for(let j=2;j<mesh.position.length;j+=3) levels.add(mesh.position[j]);
for(let i=0;i<mesh.indices.length;i+=3){
 const p=Array.from(mesh.indices.slice(i,i+3),v=>Array.from(mesh.position.slice(v*3,v*3+3)));
 const [a,b,c]=p;
 volume+=(a[0]*(b[1]*c[2]-b[2]*c[1])+a[1]*(b[2]*c[0]-b[0]*c[2])+a[2]*(b[0]*c[1]-b[1]*c[0]))/6;
}
assert.equal(JSON.stringify(polygon),before);
assert.deepEqual([...levels].sort((a,b)=>a-b),[0,4]);
assert.ok(Math.abs(volume-28)<1e-5);
assert.equal(mesh.normal.length,mesh.position.length);
for(const n of mesh.normal)assert.ok(Number.isFinite(n));
console.log('BASELINE_EXTRUSION hole-area=1 volume=28 z-levels=0,4 finite-normals');
const bevel=extrudePolygon([polygon],{depth:4,bevelSize:0.1,bevelSegments:2,smoothSide:false,smoothBevel:true});
for(const a of [bevel.position,bevel.normal,bevel.uv])for(const n of a)assert.ok(Number.isFinite(n));
for(const i of bevel.indices)assert.ok(i<bevel.position.length/3);
console.log('BASELINE_BEVEL finite-geometry indices-in-range');
