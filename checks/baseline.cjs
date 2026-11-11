const assert=require('node:assert/strict'),fs=require('node:fs');
const {decode,decoder,packet}=require('./witness.cjs');
assert.deepEqual(Array.from(decode(1,packet([[[10],[20],[30],[40]]],false))),[10,20,30,40]);
assert.deepEqual(Array.from(decode(1,packet([[[10],[20]]]))),[10,20]);
assert.deepEqual(Array.from(decode(3,packet([[[10],[20]],[[30,40],[50,60]]]))),[10,30,40,20,50,60]);
console.log('BASELINE full_mono=10,20,30,40 partial_mono=10,20 sce_cpe=10,30,40,20,50,60');
const compressed=fs.readFileSync(__dirname+'/compressed.alac'),pcm=fs.readFileSync(__dirname+'/compressed.pcm');
const tail=packet([[[1,-1],[2,-2],[3,-3],[4,-4]]]);const d=decoder(2,Buffer.concat([compressed,compressed,tail]),4096);
for(let j=0;j<2;j++){const out=d.readChunk();assert.equal(out.length,1024);for(let i=0;i<1024;i++)assert.equal(out[i],pcm.readInt16LE(i*2));}
const end=d.readChunk();assert.deepEqual(Array.from(end),[1,-1,2,-2,3,-3,4,-4]);
console.log('BASELINE compressed_packets=2 exact_samples_per_packet=1024 returned_samples_per_packet=1024 tail_exact=1');
