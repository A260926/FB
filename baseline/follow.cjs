const assert=require('node:assert/strict');
const http=require('node:http');
const scrapeIt=require('../');
const seen=[];
const server=http.createServer((req,res)=>{
 seen.push(req.url);res.setHeader('Content-Type','text/html');
 res.end(req.url==='/detail'?'<h1>Detail</h1>':'<article><b>A</b><a href="/detail">read</a></article><article><b>B</b><a href="/detail">read</a></article>');
});
server.listen(0,'127.0.0.1',async()=>{try{
 const root=`http://127.0.0.1:${server.address().port}/index`;
 const result=await scrapeIt(root,{rows:{listItem:'article',data:{name:'b',detail:{selector:'a',attr:'href',follow:{data:{title:'h1'}}}}}});
 assert.deepEqual(result.data.rows,[{name:'A',detail:'/detail'},{name:'B',detail:'/detail'}]);
 assert.deepEqual(seen,['/index']);assert.ok(result.body.includes('<article>'));
 console.log('FOLLOW_BASELINE_OK rows=2 detail=raw_href requests=1');
 }catch(e){console.error(e);process.exitCode=1;}finally{server.close();}});
