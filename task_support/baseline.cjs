'use strict';
const assert = require('node:assert/strict');
const nurbs = require('../nurbs');
const points = [], weights=[];
for(let i=0;i<3;i++) {
 points[i]=[];weights[i]=[];
 for(let j=0;j<3;j++){points[i][j]=[i/2,j/2,.2*i*(i-1)/2+.15*i*j/4+.1*j*(j-1)/2];weights[i][j]=1+.07*j;}
}
const knots=[[0,0,0,1,1,1],[0,0,0,1,1,1]];
const s=nurbs({points,weights,degree:[2,2],knots,boundary:['clamped','clamped']});
const u=.37,v=.61,p=s.evaluate([],u,v),du=s.evaluator([1,0])([],u,v),dv=s.evaluator([0,1])([],u,v);
function b(t){return [(1-t)*(1-t),2*t*(1-t),t*t];}
function db(t){return [-2*(1-t),2-4*t,2*t];}
const bu=b(u),bv=b(v),duB=db(u),dvB=db(v);let w=0,wu=0,wv=0;const a=[0,0,0],au=[0,0,0],av=[0,0,0];
for(let i=0;i<3;i++)for(let j=0;j<3;j++){
 const z=weights[i][j];w+=bu[i]*bv[j]*z;wu+=duB[i]*bv[j]*z;wv+=bu[i]*dvB[j]*z;
 for(let k=0;k<3;k++){a[k]+=bu[i]*bv[j]*z*points[i][j][k];au[k]+=duB[i]*bv[j]*z*points[i][j][k];av[k]+=bu[i]*dvB[j]*z*points[i][j][k];}
}
for(let k=0;k<3;k++) {assert.ok(Math.abs(p[k]-a[k]/w)<1e-12);assert.ok(Math.abs(du[k]-(au[k]-p[k]*wu)/w)<1e-12);assert.ok(Math.abs(dv[k]-(av[k]-p[k]*wv)/w)<1e-12);}
const basis=s.evaluator([0,0],true),basisU=s.evaluator([1,0],true);let sum=0,sumU=0;
for(let i=0;i<3;i++)for(let j=0;j<3;j++){sum+=basis(u,v,i,j);sumU+=basisU(u,v,i,j);}
assert.ok(Math.abs(sum-1)<1e-12);assert.ok(Math.abs(sumU)<1e-12);
const support=s.support([],u,v);assert.equal(support.length,18);
const t=nurbs({points:JSON.parse(JSON.stringify(points)),weights:JSON.parse(JSON.stringify(weights)),degree:[2,2],knots,boundary:['clamped','clamped']});
t.transform([1,0,0,0,0,1,0,0,0,0,1,0,.2,-.1,.3,1]);const moved=t.evaluate([],u,v);
for(let k=0;k<3;k++)assert.ok(Math.abs(moved[k]-p[k]-[.2,-.1,.3][k])<1e-12);
assert.deepEqual(s.evaluate([],u,v),p);
const poly=nurbs({points,degree:[2,2],knots,boundary:['clamped','clamped']});
assert.ok(Math.abs(poly.evaluator([2,0])([],u,v)[2]-.4)<1e-12);
assert.ok(Math.abs(poly.evaluator([1,1])([],u,v)[2]-.15)<1e-12);
console.log('NURBS_BASELINE point='+p.join(',')+' basis_sum='+sum+' derivative_sum='+sumU+' support='+support.length);
console.log('BASELINE_COMPLETE');
