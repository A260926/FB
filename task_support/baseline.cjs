'use strict';
const assert = require('node:assert/strict');
const http = require('node:http');
const Crawler = require('../lib');
(async () => {
 const wire=[], delivered=[];
 const server=http.createServer((req,res)=>{
  let body=[];req.on('data',b=>body.push(b));req.on('end',()=>{
   wire.push({method:req.method,path:req.url,body:Buffer.concat(body).toString('hex')});
   res.setHeader('Content-Type','text/html');
   res.end(req.url==='/'?'<a href="/child">child</a>':'CHILD');
  });
 });
 await new Promise(r=>server.listen(0,'127.0.0.1',r));
 const root=`http://127.0.0.1:${server.address().port}`;
 const c=new Crawler(root+'/');c.interval=1;c.respectRobotsTxt=false;c.acceptCookies=false;
 c.on('fetchcomplete',(item,data)=>delivered.push(item.path));
 let duplicates=0;c.on('queueduplicate',()=>duplicates++);
 c.queueURL(root+'/child');c.queueURL(root+'/child');
 try {
  await new Promise((resolve,reject)=>{
   const timer=setTimeout(()=>{c.stop(true);reject(new Error('crawl stalled'));},5000);
   c.on('fetchclienterror',(_,e)=>{clearTimeout(timer);reject(e)});
   c.on('complete',()=>{clearTimeout(timer);resolve()});c.start();
  });
  assert.deepEqual(wire.map(r=>r.method),['GET','GET']);
  assert.deepEqual(delivered.slice().sort(),['/','/child']);assert.equal(duplicates,2);
  console.log('GET_DISCOVERY_OK '+JSON.stringify(wire));
  console.log('URL_ONLY_DUPLICATE '+duplicates);
  console.log('QUEUE_REQUEST_API '+typeof c.queueRequest);
 } finally {c.stop(true);await new Promise(r=>server.close(r));}
})().catch(e=>{console.error(e);process.exitCode=1});
