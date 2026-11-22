'use strict';
const assert=require('node:assert/strict');
const load=require('./load.cjs'),{Bits}=require('./fixture.cjs');
const b=new Bits();
b.start(0xb3).put(16,12).put(16,12).put(1,4).put(3,4).put(100,18).put(1,1).put(20,10).put(0,1).put(0,1).put(0,1);
b.start(0).put(0,10).put(1,3).put(65535,16).put(0,1).start(1).put(8,5).put(0,1).code('1').code('1');
for(let k=0;k<6;k++){b.code(k<4?'100':'00');if(k===0)b.code('110');b.code('10');}
const J=load(),dec=new J.Decoder.MPEG1Video({decodeFirstFrame:false});let frames=0;
dec.connect({resize(){},render(y){frames++;assert.equal(Math.min(...y),125);assert.equal(Math.max(...y),131);}});
dec.write(0,b.start(0xb7).bytes());assert.equal(dec.decode(),true);assert.equal(frames,1);
console.log('NONFLAT_OK frames=1 y=125..131');
