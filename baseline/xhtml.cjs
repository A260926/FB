const assert=require('node:assert/strict');
const {Readable}=require('node:stream');
const FeedParser=require('../');
const xml='<feed xmlns="http://www.w3.org/2005/Atom"><title>Feed</title><entry><id>a</id><title>Plain</title><summary type="xhtml"><div xmlns="http://www.w3.org/1999/xhtml"><p title="A &quot; B">A &amp; B &lt; C</p><summary>inner</summary><p>TAIL</p></div></summary></entry><entry><id>b</id><title>Next</title></entry></feed>';
async function parse(parts){const p=new FeedParser();const result=[];Readable.from(parts).pipe(p);for await(const row of p)result.push(row);return result;}
(async()=>{
 const normal=await parse([xml]);const split=await parse([...Buffer.from(xml)].map(x=>Buffer.from([x])));
 assert.equal(normal.length,2);assert.equal(normal[1].title,'Next');
 assert.deepEqual(split.map(x=>x.summary),normal.map(x=>x.summary));
 const plain=await parse([xml.replace('<summary>inner</summary>','')]);
 assert.ok(plain[0].summary.includes('A & B < C'));
 assert.ok(plain[0].summary.includes('title="A " B"'));
 assert.ok(normal[0]['atom:summary'].div.summary['#'].includes('inner'));
 assert.equal(normal[0].summary,null);

 console.log('XHTML_BASELINE_OK',JSON.stringify(normal.map(x=>({guid:x.guid,summary:x.summary}))));
})().catch(e=>{console.error(e);process.exitCode=1;});
