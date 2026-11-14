import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
import {decompile} from './harness.mjs';
const normal={arch:'x86',bits:64,graph:[{name:'normal',blocks:[{offset:4096,ops:[{offset:4096,opcode:'mov eax, 7',bytes:'b807000000',size:5,type:'mov'},{offset:4101,opcode:'ret',bytes:'c3',size:1,type:'ret'}]}]}]};
const n=decompile(normal);assert.match(n.text,/\b7\b/);assert.match(n.text,/\breturn\b/);console.log(n.text);console.log('NORMAL_OK');
const sample=JSON.parse(readFileSync(new URL('./samples/switch.json',import.meta.url)));
const bytes=readFileSync(new URL('./samples/switch.bin',import.meta.url));
assert.equal(createHash('sha256').update(bytes).digest('hex'),sample.image.sha256);
for(const block of sample.graph[0].blocks) for(const op of block.ops){
 const raw=bytes.subarray(op.offset-sample.image.base,op.offset-sample.image.base+op.size).toString('hex');assert.equal(raw,op.bytes);
}
const s=decompile(sample);assert.ok(s.lines.length>0);console.log(s.text);
console.log('SWITCH_TEXT_COUNT='+((s.text.match(/\bswitch\s*\(/g)||[]).length));
console.log('BASELINE_OK real session/architecture/controlflow/print pipeline');
