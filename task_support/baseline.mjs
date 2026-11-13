import assert from 'node:assert/strict';
import { Parser } from '../dist/Parser.js';
function observe(html) {
 const tags=[]; let text=''; const events=[];
 const p=new Parser({onopentag(n,a){tags.push([n,a.id??'']);},ontext(t){text+=t;},onclosetag(n,i){events.push([n,i]);}});
 p.end(html); return {tags,text,events};
}
const ordinary=observe('<article><h2>Acme &amp; Co</h2><p id="real">ok</p></article>');
assert.equal(ordinary.text,'Acme & Cook'); assert(ordinary.tags.some(x=>x[1]==='real'));
console.log('BASELINE_PARSE_OK');
const input='<script><!--<script></script><p id="ghost">x</p>--></script><p id="real">ok</p>';
const actual=observe(input); const ghosts=actual.tags.filter(x=>x[1]==='ghost').length;
console.log(JSON.stringify({input,expected:{ghost:0,real:1},actual}));
console.log(`SCRIPT_WITNESS ghost=${ghosts} expected=0`);
if (ghosts!==1) throw new Error('Original-source witness changed; inspect source identity');
