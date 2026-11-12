// Small public-domain input fixture, generated without font libraries.
import fs from 'node:fs';
const u16=n=>{let b=Buffer.alloc(2);b.writeUInt16BE(n);return b;};
const u32=n=>{let b=Buffer.alloc(4);b.writeUInt32BE(n>>>0);return b;};
const cat=(...a)=>Buffer.concat(a.flat().map(x=>typeof x==='number'?Buffer.from([x]):x));
const integer=n=>cat(29,u32(n));
const num=n=>n>=-107&&n<=107?cat(n+139):cat(28,u16(n&65535));
const index=items=>items.length?cat(u16(items.length),1,Buffer.from([1,...items.map((_,i)=>1+items.slice(0,i+1).reduce((s,b)=>s+b.length,0))]),...items):u16(0);
const end=cat(14);
const triangle=cat(num(100),num(0),21,num(400),num(0),num(-200),num(600),num(-200),num(-600),5,14);
// A genuine cubic with an interior y maximum; no TrueType/glyf fallback.
const arch=cat(num(0),num(0),21,num(0),num(800),num(600),num(0),num(0),num(-800),8,num(-600),num(0),5,14);
const chars=index([end,triangle,arch]);
const name=index([Buffer.from('SampleCubic')]);
const topAt=4+name.length;
const priv=cat(integer(700),20);
let top=index([cat(integer(0),17,integer(priv.length),integer(0),18)]);
const charOffset=topAt+top.length+4; // empty String INDEX and GlobalSubr INDEX
const cff=cat(1,0,4,4,name,index([cat(integer(charOffset),17,integer(priv.length),integer(charOffset+chars.length),18)]),u16(0),u16(0),chars,priv);
const head=Buffer.alloc(54);head.writeUInt32BE(0x10000,0);head.writeUInt32BE(0x5f0f3cf5,12);head.writeUInt16BE(1000,18);head.writeInt16BE(600,40);head.writeInt16BE(800,42);
const hhea=Buffer.alloc(36);hhea.writeUInt32BE(0x10000,0);hhea.writeInt16BE(800,4);hhea.writeInt16BE(-200,6);hhea.writeUInt16BE(700,10);hhea.writeUInt16BE(3,34);
const maxp=cat(u32(0x5000),u16(3));
const hmtx=cat(u16(700),u16(0),u16(700),u16(100),u16(700),u16(0));
const cmapsub=cat(u16(6),u16(14),u16(0),u16(65),u16(2),u16(1),u16(2));
const cmap=cat(u16(0),u16(1),u16(3),u16(1),u32(12),cmapsub);
const post=Buffer.alloc(32);post.writeUInt32BE(0x30000,0);
const tables={'CFF ':cff,cmap,head,hhea,hmtx,maxp,name:cat(u16(0),u16(0),u16(6)),post};
const checksum=b=>{let n=0;for(let i=0;i<b.length;i+=4){let w=Buffer.alloc(4);b.copy(w,0,i,i+4);n=(n+w.readUInt32BE())>>>0;}return n;};
let offset=12+16*Object.keys(tables).length;const records=[],data=[];
for(const key of Object.keys(tables).sort()){const b=tables[key];records.push(cat(Buffer.from(key),u32(checksum(b)),u32(offset),u32(b.length)));const padding=Buffer.alloc((4-b.length%4)%4);data.push(b,padding);offset+=b.length+padding.length;}
let font=cat(Buffer.from('OTTO'),u16(records.length),u16(128),u16(3),u16(0),records,data);
const headRecord=Object.keys(tables).sort().indexOf('head');const headOffset=font.readUInt32BE(12+16*headRecord+8);
font.writeUInt32BE((0xb1b0afba-checksum(font))>>>0,headOffset+8);
fs.writeFileSync(process.argv[2],font);console.log('CFF_FIXTURE_WRITTEN bytes='+font.length);
