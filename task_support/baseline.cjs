'use strict';
const assert = require('node:assert/strict');
const nurbs = require('../nurbs');
const sample = require('../extras/sample');
const points = [
 [[0,0,1],[.5,0,1],[1,0,0]],
 [[0,0,1],[.5,.5,1],[1,1,0]],
 [[0,0,1],[0,.5,1],[0,1,0]]
];
const weights=[[1,1,1],[.7,.7,.7],[1,1,1]];
const s=nurbs({points,weights,degree:[2,2],boundary:['clamped','clamped'],knots:[[0,0,0,1,1,1],[0,0,0,1,1,1]]});
const before=JSON.stringify({points,weights});
const mesh=sample({},s,{resolution:[4,4]});
assert.equal(mesh.positions.length,75);assert.equal(mesh.cells.length,96);
let zeroArea=0,zeroNormal=0;
for(let k=0;k<mesh.cells.length;k+=3){
 const a=mesh.cells[k]*3,b=mesh.cells[k+1]*3,c=mesh.cells[k+2]*3;
 const x=[0,1,2].map(d=>mesh.positions[b+d]-mesh.positions[a+d]);
 const y=[0,1,2].map(d=>mesh.positions[c+d]-mesh.positions[a+d]);
 const area=Math.hypot(x[1]*y[2]-x[2]*y[1],x[2]*y[0]-x[0]*y[2],x[0]*y[1]-x[1]*y[0]);
 if(area<1e-13)zeroArea++;
}
for(let k=0;k<mesh.normals.length;k+=3)if(Math.hypot(...mesh.normals.slice(k,k+3))<1e-13)zeroNormal++;
const u=.37,v=.61,p=s.evaluate([],u,v),du=s.evaluator([1,0])([],u,v),dv=s.evaluator([0,1])([],u,v);
assert.ok([...p,...du,...dv].every(Number.isFinite));
const h=1e-6;
for(let d=0;d<3;d++)assert.ok(Math.abs(du[d]-(s.evaluate([],u+h,v)[d]-s.evaluate([],u-h,v)[d])/(2*h))<1e-8);
assert.equal(JSON.stringify({points,weights}),before);
let available=false;
try { available=s.evaluator([1,1])([],u,v).every(Number.isFinite); } catch(e) {
 if(!/Analytical derivative not implemented/.test(e.message))throw e;
}
console.log('PRODUCTION_EVALUATE_SAMPLE_OK vertices=25 triangles=32');
console.log(`LEGACY_POLE_ZERO_TRIANGLES=${zeroArea} ZERO_NORMALS=${zeroNormal}`);
console.log(`RATIONAL_SECOND_ORDER_AVAILABLE=${available}`);
