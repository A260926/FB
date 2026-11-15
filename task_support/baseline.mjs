import assert from 'node:assert/strict';
import {GLTFAnimation} from '../src/extras/GLTFAnimation.js';
import {makeEditingRig} from './fixture.mjs';
const {gl,geometry,root,helper,a,b,skin}=makeEditingRig();
const key=(node,transform,values)=>({node,transform,interpolation:'LINEAR',times:new Float32Array([0,1]),values:new Float32Array(values)});
const animation=new GLTFAnimation([
    key(helper,'position',[1,0,0,1,.5,0]),
    key(a,'quaternion',[0,0,0,1,Math.sin(.4),0,0,Math.cos(.4)]),
    key(b,'quaternion',[0,0,0,1,0,Math.sin(-.3),0,Math.cos(-.3)])
]);
const add=(p,q)=>p.map((v,i)=>v+q[i]);
function rotate(p,axis,t){const c=Math.cos(t),s=Math.sin(t),[x,y,z]=p;return axis===0?[x,c*y-s*z,s*y+c*z]:axis===1?[c*x+s*z,y,-s*x+c*z]:[c*x-s*y,s*x+c*y,z];}
function ref(p,j,t){
    let v=add(p,[-.3,j===0?-.2:-1.2,0]);
    if(j===1)v=add(rotate(v,1,-.6*t),[0,1,0]);
    v=add(rotate(v,0,.8*t),[.4,0,0]);
    return add(rotate(add(v,[1,.5*t,0]),2,.3),[2,-1,.5]);
}
const apply=(m,p,o=0)=>[0,1,2].map(i=>m[o+i]*p[0]+m[o+4+i]*p[1]+m[o+8+i]*p[2]+m[o+12+i]);
const near=(a,b)=>a.forEach((v,i)=>assert(Math.abs(v-b[i])<3e-6,`${v} != ${b[i]}`));
const {position,skinWeight}=geometry.attributes,p=position.data,w=skinWeight.data;
const bytes=Buffer.from(p.buffer,p.byteOffset,p.byteLength).toString('hex');
const originalKeys=animation.data.map(c=>Array.from(c.values));
let mixed=[],separation=0;
for(const t of [.2,.65,.9]){
    animation.elapsed=t;animation.update(1,true);root.updateMatrixWorld();skin.updateUniforms();
    const vertices=[];
    for(let v=0;v<3;v++){
        const point=Array.from(p.slice(v*3,v*3+3)),actual=[0,0,0],expected=[0,0,0];
        for(let j=0;j<2;j++){
            const got=apply(skin.boneMatrices,point,j*16),want=ref(point,j,t);
            near(got,want);
            for(let d=0;d<3;d++){actual[d]+=w[v*4+j]*got[d];expected[d]+=w[v*4+j]*want[d];}
        }
        near(actual,expected);vertices.push(actual);
    }
    const bary=[.2,.3,.5];mixed=[0,0,0];
    for(let v=0;v<3;v++)for(let d=0;d<3;d++)mixed[d]+=bary[v]*vertices[v][d];
    const rest=[0,0,0],average=[0,0];
    for(let v=0;v<3;v++){
        for(let d=0;d<3;d++)rest[d]+=bary[v]*p[v*3+d];
        for(let j=0;j<2;j++)average[j]+=bary[v]*w[v*4+j];
    }
    const wrong=[0,0,0];
    for(let j=0;j<2;j++){const v=apply(skin.boneMatrices,rest,j*16);for(let d=0;d<3;d++)wrong[d]+=average[j]*v[d];}
    separation=Math.max(separation,Math.hypot(...wrong.map((v,i)=>v-mixed[i])));
    assert.equal(skin.program.uniforms.boneTexture.value,skin.boneTexture);
}
assert(separation>.01,'fixture must distinguish averaging before skinning');
assert.deepEqual(animation.data.map(c=>Array.from(c.values)),originalKeys);
assert.equal(Buffer.from(p.buffer,p.byteOffset,p.byteLength).toString('hex'),bytes);
const upload=gl.uploads.find(x=>x.buffer===position.buffer);assert.equal(upload.bytes.byteLength,36);
assert.deepEqual(upload.bytes,Buffer.from(p.buffer,p.byteOffset,p.byteLength));
assert.equal(skin.boneTexture.needsUpdate,true);
// Presence is informative only: the baseline remains valid after implementation.
console.log(`OGL_MOTION_SOURCE_OK frames=3 joints=2 helper=animated inverse_bind=nonidentity upload_bytes=36`);
console.log(`SURFACE_REFERENCE separation=${separation.toFixed(6)} last=${mixed.map(x=>x.toFixed(6)).join(',')}`);
console.log(`EDIT_API ${typeof animation.editSkinMotion} SURFACE_API ${typeof skin.sampleSurface}`);
