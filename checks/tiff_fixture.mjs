import {deflateSync} from 'node:zlib';
// Public-domain small uncompressed classic-TIFF input writer. No decoder logic.
export function floatTiff({width=2,height=2,samples=3,bits=32,little=true,planar=false,rows=1,alpha=2,tileWidth=0,tileHeight=0,compression=1,values}={}) {
 if(!values||values.length!==width*height*samples)throw Error('fixture values');
 const chunks=[],unit=bits/8;
 const tiled=tileWidth>0&&tileHeight>0;
 const planes=planar?samples:1;
 if(tiled){for(let c=0;c<planes;c++)for(let y=0;y<height;y+=tileHeight)for(let x=0;x<width;x+=tileWidth){const b=new Uint8Array(tileWidth*tileHeight*unit*(planar?1:samples)),v=new DataView(b.buffer);let p=0;for(let yy=0;yy<tileHeight;yy++)for(let xx=0;xx<tileWidth;xx++)for(let ch=0;ch<(planar?1:samples);ch++){const f=(x+xx<width&&y+yy<height)?values[((y+yy)*width+x+xx)*samples+(planar?c:ch)]:0;bits===32?v.setFloat32(p,f,little):v.setFloat64(p,f,little);p+=unit;}chunks.push(b);}}
 else {for(let c=0;c<planes;c++)for(let y=0;y<height;y+=rows){let n=Math.min(rows,height-y);const b=new Uint8Array(width*n*unit*(planar?1:samples)),v=new DataView(b.buffer);let p=0;for(let yy=y;yy<y+n;yy++)for(let x=0;x<width;x++)for(let ch=0;ch<(planar?1:samples);ch++){let f=values[(yy*width+x)*samples+(planar?c:ch)];bits===32?v.setFloat32(p,f,little):v.setFloat64(p,f,little);p+=unit;}chunks.push(b);}}
 for(let i=0;i<chunks.length;i++){
  if(compression===8)chunks[i]=new Uint8Array(deflateSync(chunks[i]));
  else if(compression===32773){const a=[];for(let p=0;p<chunks[i].length;p+=128){const n=Math.min(128,chunks[i].length-p);a.push(n-1,...chunks[i].subarray(p,p+n));}chunks[i]=Uint8Array.from(a);}
  else if(compression!==1)throw Error('fixture compression');
 }
 const fields=[[256,4,[width]],[257,4,[height]],[258,3,Array(samples).fill(bits)],[259,3,[compression]],[262,3,[samples===1?1:2]],[273,4,Array(chunks.length).fill(0)],[277,3,[samples]],[278,4,[rows]],[279,4,chunks.map(b=>b.length)],[284,3,[planar?2:1]],[339,3,Array(samples).fill(3)]];
 if(tiled){fields.find(f=>f[0]===273)[0]=324;fields.find(f=>f[0]===279)[0]=325;fields.splice(fields.findIndex(f=>f[0]===278),1);fields.push([322,4,[tileWidth]],[323,4,[tileHeight]]);}
 if(samples===4)fields.push([338,3,[alpha]]);fields.sort((a,b)=>a[0]-b[0]);
 let next=8+2+fields.length*12+4;
 const outOfLine=new Map();for(const [tag,type,vals] of fields){const size=vals.length*(type===3?2:4);if(size>4){outOfLine.set(tag,next);next+=size+(size%2);}}
 const offsets=[];for(const b of chunks){offsets.push(next);next+=b.length;}
 fields.find(x=>x[0]===(tiled?324:273))[2]=offsets;
 const bytes=new Uint8Array(next),dv=new DataView(bytes.buffer);bytes.set(little?[73,73]:[77,77]);dv.setUint16(2,42,little);dv.setUint32(4,8,little);dv.setUint16(8,fields.length,little);
 fields.forEach(([tag,type,vals],i)=>{let p=10+i*12;dv.setUint16(p,tag,little);dv.setUint16(p+2,type,little);dv.setUint32(p+4,vals.length,little);let d=outOfLine.get(tag);if(d!==undefined)dv.setUint32(p+8,d,little);else d=p+8;vals.forEach((v,j)=>type===3?dv.setUint16(d+j*2,v,little):dv.setUint32(d+j*4,v,little));});
 chunks.forEach((b,i)=>bytes.set(b,offsets[i]));return bytes.buffer;
}
