const path = require('node:path');
process.env.NODE_PATH=path.join(__dirname,'node_modules');require('node:module').Module._initPaths();
require('coffee-script/register');
const AV=require('av'), ALAC=require('../src/decoder.coffee');
function cookie(channels,frames=4){const b=Buffer.alloc(24);b.writeUInt32BE(frames);b[5]=16;b[6]=40;b[7]=10;b[8]=14;b[9]=channels;b.writeUInt16BE(255,10);b.writeUInt32BE(44100,20);return b;}
function packet(elements,partial=true){const bits=[];const w=(v,n)=>{for(let i=n-1;i>=0;i--)bits.push((v>>>i)&1);};
 for(const samples of elements){let ch=samples[0].length;w(ch===2?1:0,3);w(0,4);w(0,12);w(partial?1:0,1);w(0,2);w(1,1);if(partial)w(samples.length,32);for(const frame of samples)for(const x of frame)w(x&65535,16);}
 w(7,3);while(bits.length%8)bits.push(0);const b=Buffer.alloc(bits.length/8);bits.forEach((v,i)=>b[i>>3]|=v<<(7-(i%8)));return b;}
function decode(channels,data,frames=4){const demuxer=new AV.EventEmitter();const d=new ALAC(demuxer,{});d.setCookie(new AV.Buffer(cookie(channels,frames)));demuxer.emit('data',new AV.Buffer(data));return d.readChunk();}
function decoder(channels,data,frames=4){const demuxer=new AV.EventEmitter();const d=new ALAC(demuxer,{});d.setCookie(new AV.Buffer(cookie(channels,frames)));demuxer.emit('data',new AV.Buffer(data));return d;}
module.exports={decode,decoder,packet,cookie,AV,ALAC};
