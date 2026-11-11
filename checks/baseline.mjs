import assert from 'node:assert/strict';
import { BufferGeometry, Float32BufferAttribute, Matrix4 } from 'three';
import { MeshBVH } from '../src/core/MeshBVH.js';
function geometry(xs) {
 const g = new BufferGeometry();
 g.setAttribute('position',new Float32BufferAttribute(xs.flatMap(x=>[x,0,0,x,1,0,x,0,1]),3));
 return g;
}
const a=geometry([0]);
const b=geometry([90,70,50,30,80,60,40,2]);
const tree=new MeshBVH(a,{targetLeafSize:1});
const matrix=new Matrix4().makeScale(.01,1,1);
const plain=tree.closestPointToGeometry(b,matrix,{}, {},0,.1);
assert.ok(Math.abs(plain.distance-.02)<1e-6);
console.log('PLAIN_DISTANCE_OK',plain.distance);
b.boundsTree=new MeshBVH(b,{targetLeafSize:1,indirect:true});
let accelerated;
try { accelerated=tree.closestPointToGeometry(b,matrix,{}, {},0,.1); }
catch(e) { console.log('ACCELERATED_ERROR',e.message); }
console.log('ACCELERATED_DISTANCE',accelerated?.distance??null);
assert.ok(accelerated!==null && Math.abs(accelerated.distance-plain.distance)<1e-6);
console.log('BASELINE_BVH_OK nonuniform-accelerated-matches-plain');
const c=geometry([0,4,2,6]);
const d=geometry([1,5,3,7]);
const ct=new MeshBVH(c,{targetLeafSize:1,indirect:true});
d.boundsTree=new MeshBVH(d,{targetLeafSize:1});
try { ct.closestPointToGeometry(d,new Matrix4()); console.log('MIXED_MODE accepted'); }
catch(e) { console.log('MIXED_MODE_ERROR',e.message); }
