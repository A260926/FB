import assert from 'node:assert/strict';
import * as p from '../src/programs.js';
const calls=[];
function context(rowMajor) {
 const gl={UNIFORM_TYPE:0x8a37,UNIFORM_SIZE:0x8a38,UNIFORM_BLOCK_INDEX:0x8a3a,UNIFORM_OFFSET:0x8a3b,
 UNIFORM_ARRAY_STRIDE:0x8a3c,UNIFORM_MATRIX_STRIDE:0x8a3d,UNIFORM_IS_ROW_MAJOR:0x8a3e,
 getProgramParameter:(_,n)=>n===0x8b86?1:1,
 getActiveUniform:()=>({name:'Scene.m',type:0x8b65,size:1}),
 getActiveUniforms:(_,idx,n)=>idx.map(()=>({[0x8a37]:0x8b65,[0x8a38]:1,[0x8a3a]:0,[0x8a3b]:0,[0x8a3c]:0,[0x8a3d]:16,[0x8a3e]:rowMajor}[n])),
 getActiveUniformBlockName:()=> 'Scene',getUniformBlockIndex:()=>0,
 getActiveUniformBlockParameter:(_,i,n)=>n===0x8a40?48:n===0x8a43?[0]:true,
 createBuffer:()=>({}),bindBuffer(){},bufferData(){},uniformBlockBinding(){},
 bindBufferRange:(...a)=>calls.push(['range',...a]),
 bufferSubData:(t,d,v,s,n)=>calls.push(['upload',d,s,n,Array.from(v.slice(s,s+n))])};
 return gl;
}
const values=[1,2,3,4,5,6];
const gl=context(false),spec=p.createUniformBlockSpecFromProgram(gl,{});
const normal=p.createUniformBlockInfoFromProgram(gl,{},spec,'Scene');
p.setBlockUniforms(normal,{m:values});assert.deepEqual([...normal.asFloat.slice(0,8)],[1,2,3,0,4,5,6,0]);
const rowgl=context(true),rowspec=p.createUniformBlockSpecFromProgram(rowgl,{});
const array=new ArrayBuffer(224);
const row=p.createUniformBlockInfoFromProgram(rowgl,{},rowspec,'Scene',{array,offset:16,buffer:{},bufferOffset:64});
p.setBlockUniforms(row,{m:values});
assert.deepEqual([...new Float32Array(array,16,12)],[1,2,3,0,4,5,6,0,0,0,0,0]);
p.setUniformBlock(rowgl,rowspec,row);
const up=calls.find(a=>a[0]==='upload');assert.equal(up[1],0);assert.equal(up[2],64);assert(up[4].every(v=>v===0));
console.log('TWGL_BASELINE_OK column-major=correct row-major=mispacked cpu16-gpu64=wrong-source');
