import assert from 'node:assert/strict';
import { BoxGeometry, Geometry, Buffer, Attribute, BasicMaterial, WebGLRenderer } from '../src/main.js';
import { WebGLVertexArrayBindings } from '../src/webgl/WebGLVertexArrayBindings.js';
const calls=[];
const gl={ARRAY_BUFFER:34962,ELEMENT_ARRAY_BUFFER:34963,UNSIGNED_INT:5125,INT:5124,
 createVertexArray:()=>({}),bindVertexArray:v=>calls.push(['vao',v]),deleteVertexArray:()=>{},
 enableVertexAttribArray:i=>calls.push(['enable',i]),vertexAttribDivisor:(...a)=>calls.push(['divisor',...a]),
 vertexAttribPointer:(...a)=>calls.push(['pointer',...a]),bindBuffer:(...a)=>calls.push(['bind',...a]),
 drawElements:(...a)=>calls.push(['elements',...a]),drawArrays:(...a)=>calls.push(['arrays',...a]),
 drawElementsInstanced:(...a)=>calls.push(['elementsInstanced',...a]),drawArraysInstanced:(...a)=>calls.push(['arraysInstanced',...a])};
const caps={version:2,getExtension:()=>null};
const buffers={get:b=>({type:b.array instanceof Uint16Array?5123:5126,bytesPerElement:b.array.BYTES_PER_ELEMENT,glBuffer:b})};
const indexed=new BoxGeometry(), material=new BasicMaterial();
const plain=new Geometry();plain.addAttribute('a_Position',new Attribute(new Buffer(new Float32Array([0,0,0,1,0,0,0,1,0]),3),3));
const bindings=new WebGLVertexArrayBindings('baseline',gl,caps,buffers);
const program={id:11,getAttributes:()=>({a_Position:{count:3,location:0,locationSize:1,format:5126}})};
bindings.setup({},indexed,program);const once=calls.filter(x=>x[0]==='pointer').length;
bindings.setup({},indexed,program);assert.equal(calls.filter(x=>x[0]==='pointer').length,once);assert.equal(once,1);
const renderer={context:gl,capabilities:caps,_buffers:buffers};
const info=[];const renderInfo={update:(...a)=>info.push(a)};
WebGLRenderer.prototype._draw.call(renderer,indexed,material,{start:3,count:6},renderInfo);
WebGLRenderer.prototype._draw.call(renderer,plain,material,null,renderInfo);
assert.deepEqual(calls.filter(x=>x[0]==='elements').at(-1),['elements',material.drawMode,6,5123,6]);
assert.deepEqual(calls.filter(x=>x[0]==='arrays').at(-1),['arrays',material.drawMode,0,3]);
indexed.instanceCount=2;plain.instanceCount=3;
WebGLRenderer.prototype._draw.call(renderer,indexed,material,{start:3,count:6},renderInfo);
WebGLRenderer.prototype._draw.call(renderer,plain,material,null,renderInfo);
assert.deepEqual(calls.filter(x=>x[0]==='elementsInstanced').at(-1),['elementsInstanced',material.drawMode,6,5123,6,2]);
assert.deepEqual(calls.filter(x=>x[0]==='arraysInstanced').at(-1),['arraysInstanced',material.drawMode,0,3,3]);
assert.deepEqual(info.map(x=>x[2]),[1,1,2,3]);
indexed.instanceCount=0;const before=calls.length;
WebGLRenderer.prototype._draw.call(renderer,indexed,material,null,renderInfo);assert.equal(calls.length,before);
bindings.releaseByGeometry(indexed);
console.log('NATIVE_DRAW_BASELINE ordinary=2 explicit_instanced=2 index_offset=6 vao_reused=1 empty_draw=0');
