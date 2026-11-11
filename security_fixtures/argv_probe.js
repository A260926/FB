'use strict';
const assert = require('node:assert/strict');
const {parse, quote} = require('..');
const contract = process.argv.includes('--contract');
const opts = {argvOnly: true};
assert.deepEqual(parse('tool "a b" \'c;d\''), ['tool', 'a b', 'c;d']);
let lookups = 0;
const malformed = 'tool $VALUE "unterminated';
if (!contract) {
  assert.deepEqual(parse('tool \\*'), ['tool', {op:'glob', pattern:'*'}]);
  console.log('legacy escaped wildcard becomes glob; no shell is executed');
  console.log('legacy malformed=' + JSON.stringify(parse(malformed, () => { lookups++; return 'data'; }, opts)));
  console.log('legacy resolver calls=' + lookups);
  console.log('legacy operator=' + JSON.stringify(parse('tool ok; other', {}, opts)));
  console.log('BASELINE OK');
  process.exit(0);
}
assert.throws(() => parse(malformed, () => { lookups++; return 'data'; }, opts), SyntaxError);
assert.equal(lookups, 0);
assert.deepEqual(parse('tool "a b" \'c;d\' \\*', {}, opts), ['tool','a b','c;d','*']);
for (const source of ['tool;other', 'tool $(other)', 'tool `other`', 'tool *', 'tool # note', 'tool\nother', 'tool $\'x\'', 'tool ${X:-y}', 'tool trailing\\']) {
  assert.throws(() => parse(source, {}, opts), SyntaxError, source);
}
assert.deepEqual(parse('tool "$VALUE" end', {VALUE:'; $(literal) * # x'}, opts), ['tool','; $(literal) * # x','end']);
assert.deepEqual(parse('tool "$MISSING" \'\' "$VALUE"', {VALUE:'a\nb'}, opts), ['tool','','','a\nb']);
assert.deepEqual(parse(quote(['tool','*','a;b','x y']), {}, opts), ['tool','*','a;b','x y']);
assert.throws(() => parse('tool $VALUE', () => ({op:';'}), opts), TypeError);
assert.throws(() => parse('tool', {}, {argvOnly:true, splitUnquoted:true}), TypeError);
assert.throws(() => parse('tool', {}, {argvOnly:true, escape:'^'}), TypeError);
console.log('CONTRACT OK');
