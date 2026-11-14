import fs from 'node:fs';
import assert from 'node:assert/strict';
import {parse} from './parse.mjs';
import {generateISD} from '../src/main/js/isd.js';
const api={generateISD};
const doc=parse(fs.readFileSync(new URL('./basic.ttml',import.meta.url),'utf8'));
function text(e){return (e.text??'')+(e.contents??[]).map(text).join('');}
const live=api.generateISD(doc,2);
assert.equal(text(live),'ABbee C');
assert.equal(text(api.generateISD(doc,3)),'');
console.log('ISD at 2s: '+text(live));
console.log('ISD at 3s: empty; Node core path doc.js + isd.js');
