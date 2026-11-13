import Crawler from '../src/index.ts';
import http from 'node:http';
import assert from 'node:assert/strict';
let slow;const seen=[];
const server=http.createServer((req,res)=>{if(req.url==='/a')slow=res;else{res.end('B');}});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
try{const c=new Crawler({maxConnections:2,retries:0,jQuery:false,silence:true});
 const drained=new Promise((resolve,reject)=>{c.once('drain',resolve);setTimeout(()=>reject(Error('baseline timeout')),5000).unref();});
 const base=`http://127.0.0.1:${server.address().port}`;
 c.add(['/a','/b'].map(path=>({url:base+path,callback:(err,res,done)=>{assert.ifError(err);seen.push(res.body.toString());if(path==='/b'){assert.ok(slow);slow.end('A');}done();}})));
 await drained;assert.deepEqual(seen,['B','A']);console.log('existing callback order: B,A; both HTTP pages delivered');
}finally{await new Promise(r=>server.close(r));}
