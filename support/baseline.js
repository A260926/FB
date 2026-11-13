'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {Transmuxer} = require('../lib/mp4/transmuxer');
const MetadataStream = require('../lib/m2ts/metadata-stream');
const id3 = require('../test/utils/id3-generator');
const tx = new Transmuxer({baseMediaDecodeTime:90000});
const segments=[];
tx.on('data', e=>segments.push(e));
tx.push(new Uint8Array(fs.readFileSync('test/segments/test-aac-segment.aac')));
tx.flush();
assert.ok(segments.length>0);
let moofs=0,mdats=0,emsgs=0,tags=0;
for(const s of segments){
 tags+=s.metadata.length;
 for(let p=0;p<s.data.length;){
  const b=Buffer.from(s.data.buffer,s.data.byteOffset,s.data.byteLength);const n=b.readUInt32BE(p);assert.ok(n>=8&&p+n<=b.length);
  const type=b.toString('ascii',p+4,p+8);if(type==='moof')moofs++;if(type==='mdat')mdats++;if(type==='emsg')emsgs++;p+=n;
 }
}
assert.ok(moofs>0&&mdats>0&&tags>0);assert.equal(emsgs,0);
const ts = new Transmuxer(); const combined=[]; ts.on('data',e=>combined.push(e)); ts.push(new Uint8Array(fs.readFileSync('test/segments/test-segment.ts'))); ts.flush(); assert.equal(combined.length,1); assert.equal(combined[0].type,'combined'); assert.ok(combined[0].data.length>1000);
const ms=new MetadataStream();const seen=[];ms.on('data',t=>seen.push(t));
const tag=id3.id3Tag(id3.id3Frame('TXXX',[0],id3.stringToCString('k'),id3.stringToCString('v')));
ms.push({type:'timed-metadata',pts:123,dts:123,dataAlignmentIndicator:true,data:new Uint8Array(tag.concat(tag))});
assert.equal(seen.length,1);
console.log('baseline: aac_segments='+segments.length+' metadata='+tags+' emsg=0 concatenated_tags='+seen.length+' ts_combined='+combined.length);
