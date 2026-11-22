'use strict';
// Independently written elementary-stream fixture: 16x16, I/P/B are flat 128.
// There is no B-decoding implementation here.
class Bits {
  constructor() {this.a=[];}
  put(v,n) {for(let i=n-1;i>=0;i--) this.a.push((v>>>i)&1); return this;}
  code(s) {for(const c of s) this.a.push(Number(c)); return this;}
  align() {while(this.a.length%8) this.a.push(0); return this;}
  start(c) {return this.align().put(1,24).put(c,8);}
  bytes() {this.align(); const out=Buffer.alloc(this.a.length/8);
    this.a.forEach((v,i)=>out[i>>3]|=v<<(7-(i%8))); return out;}
}
function stream(types=[1,2,3]) {
  const b=new Bits();
  b.start(0xb3).put(16,12).put(16,12).put(1,4).put(3,4)
    .put(100,18).put(1,1).put(20,10).put(0,1).put(0,1).put(0,1);
  types.forEach((type,i)=>{
    b.start(0).put(type===3?1:i===1?2:0,10).put(type,3).put(65535,16);
    if(type!==1) b.put(0,1).put(1,3);
    if(type===3) b.put(0,1).put(1,3);
    b.put(0,1).start(1).put(8,5).put(0,1).code('1');
    if(type===3) b.code('10').code('1111'); // bidir, four zero vector deltas
    else {
      b.code(type===1?'1':'00011');
      for(let j=0;j<6;j++) b.code(j<4?'100':'00').code('10'); // dc size0, EOB
    }
  });
  return b.start(0xb7).bytes();
}
module.exports={Bits,stream};
