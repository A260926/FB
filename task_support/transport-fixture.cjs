'use strict';
const {Bits}=require('./fixture.cjs');
// MPEG-1 Layer II, 128 kb/s stereo at 48 kHz, ISO table B.2a.
// Only subband 0 is allocated (three-level grouped); SCFSI=2, asymmetric samples.
function mp2() {
 const b=new Bits();b.put(0xfffd8400,32);
 for(let sb=0;sb<27;sb++)for(let ch=0;ch<2;ch++)b.put(sb===0?1:0,sb<11?4:sb<23?3:2);
 b.put(2,2).put(2,2).put(0,6).put(3,6);
 for(let gr=0;gr<12;gr++)b.put(gr%2?26:0,5).put(gr%2?0:26,5);
 const out=Buffer.alloc(384);b.bytes().copy(out);return out;
}
function timestamp(n) {
 const a=new Bits();a.put(2,4).put(Math.floor(n/1073741824),3).put(1,1).put(Math.floor(n/32768)%32768,15).put(1,1).put(n%32768,15).put(1,1);return a.bytes();
}
function pes(id,pts,payload) {
 const len=payload.length+8;
 return Buffer.concat([Buffer.from([0,0,1,id,len>>8,len&255,0x80,0x80,5]),timestamp(pts),payload]);
}
function packets(pid,id,pts,payload,cc=0) {
 const p=pes(id,pts,payload),out=[];
 for(let at=0;at<p.length;) {
  const n=Math.min(184,p.length-at),a=Buffer.alloc(188,0xff),start=at===0;
  a[0]=0x47;a[1]=(start?0x40:0)|(pid>>8);a[2]=pid&255;
  let pos=4;
  if(n<184) {a[3]=0x30|(cc++&15);a[4]=183-n;if(a[4])a[5]=0;pos=188-n;}
  else a[3]=0x10|(cc++&15);
  p.copy(a,pos,at,at+n);out.push(a);at+=n;
 }
 return Buffer.concat(out);
}
module.exports={mp2,packets,pes,timestamp};
