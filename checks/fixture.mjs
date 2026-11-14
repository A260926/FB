import { Skin } from '../src/extras/Skin.js';
// GL allocation only. Animation, transforms and bone matrices are the original production classes.
// Constants/allocator shape adapted from an earlier same-commit resource baseline.
export function makeSkin() {
  let id = 0, draws = 0;
  const gl = { canvas: {}, TRIANGLES:4, TEXTURE_2D:3553, UNSIGNED_BYTE:5121, FLOAT:5126,
    RGBA:6408, RGBA32F:34836, NEAREST:9728, NEAREST_MIPMAP_LINEAR:9986,
    LINEAR:9729, CLAMP_TO_EDGE:33071, REPEAT:10497, createTexture:()=>({id:++id}),
    renderer: {isWebgl2:true, parameters:{maxAnisotropy:0}, state:{}} };
  const geometry = { draw(){ draws++; } };
  const program = { uniforms:{}, use(){} };
  const rig = { bones:[{name:'root',parent:-1},{name:'child',parent:0}],
    bindPose:{position:[0,0,0, 0,1,0], quaternion:[0,0,0,1, 0,0,0,1], scale:[1,1,1, 1,1,1]} };
  const skin = new Skin(gl,{rig,geometry,program});
  return { skin, gl, program, draws:()=>draws };
}
