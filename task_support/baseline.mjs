// Reused public baseline scaffold at identical pinned commit.
import assert from 'node:assert/strict';
import http from 'node:http';
import {gzipSync} from 'node:zlib';
import Crawler from '../dist/index.js';
const raw=Buffer.concat([Buffer.from('<meta charset="utf-8"><p data-name="'),Buffer.from([0x4a,0xf6,0x72,0x67]),Buffer.from('">'),Buffer.from([0x4a,0xf6,0x72,0x67]),Buffer.from('</p>')]);
const requests=[];
const server=http.createServer((req,res)=>{
 requests.push(req.url);res.writeHead(200,{'content-type':'text/html;charset=iso-8859-1','content-encoding':'gzip'});
 const body=gzipSync(raw);res.write(body.subarray(0,9));res.end(body.subarray(9));
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
try{
 const crawler=new Crawler({retries:0,silence:true});
 const url=`http://127.0.0.1:${server.address().port}/document`;
 const result=await crawler.send({url});
 assert.equal(result.charset,'iso-8859-1');assert.equal(result.$('p').text(),'Jörg');
 assert.equal(result.$('p').attr('data-name'),'Jörg');assert.equal(typeof result.body,'string');
 const binary=await crawler.send({url,encoding:null,jQuery:false});
 assert.ok(Buffer.isBuffer(binary.body));assert.deepEqual(binary.body,raw);
 const queued=await new Promise((resolve,reject)=>crawler.add({url,callback:(error,response,done)=>{
  done();if(error)reject(error);else resolve(response);
 }}));
 assert.equal(queued.$('p').text(),'Jörg');assert.equal(requests.length,3);
 console.log('WIRE_BASELINE_OK requests=3 decoded=Jörg binary-exact=true');
}finally{await new Promise(resolve=>server.close(resolve));}
