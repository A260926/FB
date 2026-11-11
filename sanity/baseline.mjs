import assert from 'node:assert/strict';
import { BoxGeometry } from 'three';
import { Brush, Evaluator, ADDITION, SUBTRACTION, computeMeshVolume } from '../src/index.js';
function box(x) {
 const g=new BoxGeometry(); g.computeTangents();
 const b=new Brush(g); b.position.x=x; b.updateMatrixWorld(); return b;
}
const a=box(0), b=box(.3);
const e=new Evaluator();
const plain=e.evaluate(a,b,ADDITION);
assert.ok(Math.abs(computeMeshVolume(plain)-1.3)<1e-5);
console.log('CSG_VOLUME_OK',computeMeshVolume(plain));
e.attributes=['position','normal','uv','tangent'];
for(const [op,name] of [[ADDITION,'union'],[SUBTRACTION,'subtract']]) {
 const r=e.evaluate(a,b,op), g=r.geometry;
 const p=g.getAttribute('position'),t=g.getAttribute('tangent');
 const finite=Array.from(t.array).every(Number.isFinite);
 console.log('TANGENT_OBSERVED',name,p.count,t.count,finite);
 assert.ok(t.count!==p.count || !finite);
}
console.log('BASELINE_CSG_OK tangent-layout-defect-reproduced');
