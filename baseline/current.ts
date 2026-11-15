import assert from 'node:assert/strict';
import Armature from '../src/armature/Armature';
import SkinMTX from '../src/armature/skins/SkinMTX';
import Animator from '../src/animation/Animator';
import Clip from '../src/animation/Clip';
import Vec3Track from '../src/animation/tracks/Vec3Track';
const arm = new Armature();
arm.addBone('root', -1, undefined, [0,0,0]);
arm.addBone('hand', 0, undefined, [0,1,0]);
arm.bind(SkinMTX);
function clip(x:number) {
  const c = new Clip(); c.duration = 1; c.timeStamps = [new Float32Array([0,1])];
  const t = new Vec3Track(); t.boneIndex = 0; t.timeStampIndex = 0;
  t.values = new Float32Array([x,0,0,x,0,0]); c.tracks = [t]; return c;
}
const p = arm.newPose();
new Animator().setClip(clip(2)).update(0.5).applyPose(p);
p.updateWorld(); assert.equal(p.bones[1].world.pos[0], 2);
arm.updateSkinFromPose(p);
assert.equal((arm.getSkinOffsets()![0] as Float32Array)[12], 2);
new Animator().setClip(clip(8)).update(0.5).applyPose(p);
p.updateWorld(); assert.equal(p.bones[1].world.pos[0], 8);
assert.equal(p.bones[1].world.pos[1], 1);
console.log('OSSOS_CURRENT_OK root=2 then=8 handY=1 skinX=2');
