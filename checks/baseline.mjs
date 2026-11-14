import assert from 'node:assert/strict';
import './resource-baseline.mjs';
import { GLTFAnimation } from '../src/extras/GLTFAnimation.js';
import { Transform } from '../src/core/Transform.js';
import { makeSkin } from './fixture.mjs';
const close=(a,b)=>assert.ok(Math.abs(a-b)<1e-6, `${a} != ${b}`);
const {skin,draws}=makeSkin();
const root=skin.bones[0], child=skin.bones[1];
const clip=new GLTFAnimation([{node:root,transform:'position',interpolation:'LINEAR',
 times:new Float32Array([0,2]),values:new Float32Array([0,0,0,4,0,0])}]);
clip.elapsed=1;clip.update(1,true);skin.draw();
close(root.position.x,2);close(child.worldMatrix[12],2);close(child.worldMatrix[13],1);
close(skin.boneMatrices[12],2);close(skin.boneMatrices[16+12],2);
assert.equal(skin.boneTexture.needsUpdate,true);assert.equal(draws(),1);
console.log('OGL_BASELINE_LINEAR_HIERARCHY_PALETTE_OK');
// Actual legacy Skin.addAnimation -> Animation.update -> Skin.draw path.
const frame=x=>({position:[x,0,0, 0,1,0],quaternion:[0,0,0,1, 0,0,0,1],scale:[1,1,1, 1,1,1]});
const old=skin.addAnimation({frames:[frame(0),frame(4),frame(0)]});
old.elapsed=.5;skin.update();skin.draw();
close(root.position.x,2);close(skin.boneMatrices[28],2);assert.equal(draws(),2);
const n=new Transform();
const cubic=new GLTFAnimation([{node:n,transform:'position',interpolation:'CUBICSPLINE',times:[0,1],
 values:[0,0,0, 0,0,0, 0,0,0, 0,0,0, 2,0,0, 0,0,0]}]);
cubic.elapsed=.5;cubic.update(1,true);close(n.position.x,1);
console.log('OGL_BASELINE_LEGACY_ANIMATION_CUBIC_OK');
