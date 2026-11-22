'use strict';
const assert=require('node:assert/strict');
const load=require('./load.cjs'), {stream}=require('./fixture.cjs');
const J=load(), decoder=new J.Decoder.MPEG1Video({decodeFirstFrame:false});
let renders=0;
decoder.connect({resize(w,h){assert.equal(w,16);assert.equal(h,16);},
  render(y,cr,cb){for(const plane of [y,cr,cb]) {
    assert.ok(plane.length>0); assert.ok(plane.every(v=>v===128)); } renders++;}});
decoder.write(0,stream());
let consumed=0;
while(decoder.decode()) {assert.ok(++consumed<8);}
assert.equal(consumed,3); assert.equal(renders,2);
console.log('BASELINE pictures=3 rendered=2 flat_yuv=128 B_skipped=1');
