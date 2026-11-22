import assert from 'node:assert/strict';
import fs from 'node:fs';
import {compile,decompile} from '../src/index.js';
const source=fs.readFileSync(new URL('./drawing.ass',import.meta.url),'utf8');
const doc=compile(source);assert.equal(doc.width,8);assert.equal(doc.dialogues.length,1);
const f=doc.dialogues[0].slices[0].fragments[0];assert.equal(f.tag.p,1);assert.equal(f.drawing.instructions.length,2);
assert.equal(f.drawing.d,'M0,0L4,0,4,4,0,4');assert.deepEqual(doc.dialogues[0].pos,{x:1,y:1});
const again=compile(decompile(doc));assert.equal(again.dialogues[0].slices[0].fragments[0].drawing.d,f.drawing.d);
console.log('ASS_DRAWING_BASELINE_OK dialogues=1 commands=2 roundtrip=1');
