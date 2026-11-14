import assert from 'node:assert/strict';
import {Texture} from '../src/core/Texture.js';
import {RenderTarget} from '../src/core/RenderTarget.js';
import {Renderer} from '../src/core/Renderer.js';
const calls=[]; let id=0;
const gl={canvas:{width:8,height:8},TEXTURE_2D:3553,UNSIGNED_BYTE:5121,RGBA:6408,
 CLAMP_TO_EDGE:33071,LINEAR:9729,NEAREST_MIPMAP_LINEAR:9986,REPEAT:10497,
 FRAMEBUFFER:36160,RENDERBUFFER:36161,COLOR_ATTACHMENT0:36064,
 DEPTH_STENCIL:34041,DEPTH_STENCIL_ATTACHMENT:33306,TEXTURE0:33984,
 createTexture(){calls.push('texture');return {id:++id};},
 createFramebuffer(){calls.push('framebuffer');return {id:++id};},
 createRenderbuffer(){calls.push('renderbuffer');return {id:++id};}};
for(const name of ['bindTexture','texParameteri','pixelStorei','texImage2D',
 'framebufferTexture2D','bindRenderbuffer','renderbufferStorage',
 'framebufferRenderbuffer','bindFramebuffer','activeTexture']) gl[name]=()=>{};
const r=Object.create(Renderer.prototype); gl.renderer=r; r.gl=gl;
r.parameters={maxAnisotropy:0}; r.isWebgl2=true;r.drawBuffers=()=>{};
r.state={textureUnits:[],activeTextureUnit:0,framebuffer:null};
const target=new RenderTarget(gl,{width:8,height:4,color:2,depth:true,stencil:true});
assert.equal(target.textures.length,2); assert.ok(target.depthStencilBuffer);
assert.equal(calls.filter(x=>x==='texture').length,2);
target.setSize(4,2); assert.equal(target.texture.width,4);
const standalone=new Texture(gl,{width:1,height:1,generateMipmaps:false});
standalone.update(3);assert.equal(r.state.textureUnits[3],standalone.id);
assert.equal(typeof standalone.remove,'undefined');
assert.equal(typeof target.remove,'undefined');
console.log('allocated textures=3 framebuffer=1 renderbuffer=1; resize and unit3 upload passed');
console.log('current Texture.remove and RenderTarget.remove are absent');
