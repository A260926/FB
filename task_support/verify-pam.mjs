import fs from 'node:fs';import assert from 'node:assert/strict';
const b=fs.readFileSync(process.argv[2]);const end=b.indexOf(Buffer.from('ENDHDR\n'));assert.ok(end>=0);
const h=b.subarray(0,end).toString();for(const s of ['P7','WIDTH 8','HEIGHT 8','DEPTH 4','MAXVAL 255','TUPLTYPE RGB_ALPHA'])assert.ok(h.split('\n').includes(s));
assert.equal(b.length-end-7,8*8*4);console.log('DRAWING_EXAMPLE_PAM_OK');
