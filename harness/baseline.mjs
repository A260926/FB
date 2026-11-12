import assert from 'node:assert/strict';
import {Parser, parseDocument} from '../dist/index.js';
const html = '<div title="A&amp;B">x&amp;y<textarea>R&lt;Z</textarea><script>a<&amp;</script></div>';
const tags=[]; let text='';let ended=0;
const p=new Parser({onopentag:(name,a)=>tags.push([name,a]),ontext:t=>text+=t,onend:()=>ended++});
for(let i=0;i<html.length;i+=3)p.write(html.slice(i,i+3));p.end();
assert.equal(tags[0][1].title,'A&B');assert.equal(text,'x&yR<Za<&amp;');assert.equal(ended,1);
const doc=parseDocument(html);assert.equal(doc.children[0].name,'div');assert.equal(doc.children[0].attribs.title,'A&B');
console.log('HTMLPARSER_BASELINE_OK text='+text);
