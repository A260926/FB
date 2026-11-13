'use strict';
const test=require('node:test');const assert=require('node:assert/strict');const {once}=require('node:events');const WebSocket=require('ws');const CDP=require('../');
test('baseline CDP command and generated event',{timeout:10000},async()=>{
 const server=new WebSocket.Server({host:'127.0.0.1',port:0});await once(server,'listening');
 let peer;server.on('connection',socket=>{peer=socket;socket.on('message',raw=>{const m=JSON.parse(raw);socket.send(JSON.stringify({id:m.id,result:{title:'offline form'}}));socket.send(JSON.stringify({method:'Form.changed',sessionId:'child',params:{value:'Ada'}}));});});
 let client;
 try{client=await CDP({target:`ws://127.0.0.1:${server.address().port}`,protocol:{domains:[{domain:'Form',commands:[{name:'read'}],events:[{name:'changed'}]}]}});const event=client.Form.changed('child');const result=await client.Form.read();assert.equal(result.title,'offline form');assert.deepEqual(await event,{value:'Ada'});console.log('production CDP command=offline form event=Ada observe_available='+String(typeof client.observe==='function'));}
 finally{if(client)await client.close();if(peer)peer.terminate();await new Promise(r=>server.close(r));}
});
