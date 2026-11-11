'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const parse = require('..');

const RESPONSE_URL = 'https://a.test/api/items?p=1';

// ---------------------------------------------------------------------------
// parse.entries: ordering, quoting, escaping
// ---------------------------------------------------------------------------

test('entries: empty input yields an empty array', () => {
  assert.deepEqual(parse.entries(''), []);
  assert.deepEqual(parse.entries([]), []);
  assert.deepEqual(parse.entries(['']), []);
  assert.deepEqual(parse.entries(['', '']), []);
});

test('entries: ordered entries with target, rels and params', () => {
  const header =
    '<items?p=2&v=1&v=2#frag>; rel="Next prev"; title="x; y, <z>" , ' +
    '<https://b.test/wrong>; rel=next; anchor="?p=9"';

  const entries = parse.entries(header);

  assert.equal(entries.length, 2);

  assert.deepEqual(entries[0], {
    target: 'items?p=2&v=1&v=2#frag',
    rels: ['next', 'prev'],
    params: [['title', 'x; y, <z>']]
  });

  assert.deepEqual(entries[1], {
    target: 'https://b.test/wrong',
    rels: ['next'],
    params: [['anchor', '?p=9']]
  });
});

test('entries: accepts several header strings in receive order', () => {
  const entries = parse.entries([
    '<u1>; rel=next',
    '<u2>; rel=prev'
  ]);
  assert.equal(entries.length, 2);
  assert.equal(entries[0].target, 'u1');
  assert.equal(entries[1].target, 'u2');
});

test('entries: empty header strings between real ones are skipped', () => {
  const entries = parse.entries(['', '<u1>; rel=next', '', '<u2>; rel=last']);
  assert.deepEqual(entries.map((e) => e.target), ['u1', 'u2']);
});

test('entries: registered rel tokens are lower-cased, extension URIs stay verbatim', () => {
  const entries = parse.entries(
    '<u>; rel="Next https://example.com/Rel FooBar"'
  );
  assert.deepEqual(entries[0].rels, [
    'next',
    'https://example.com/Rel',
    'foobar'
  ]);
});

test('entries: rel whitespace splitting handles runs and edges', () => {
  assert.deepEqual(parse.entries('<u>; rel="  next   prev "')[0].rels,
    ['next', 'prev']);
});

test('entries: param names are lower-cased and ordered with duplicates kept', () => {
  const entries = parse.entries('<u>; B=2; a=1; b=3; rel=next');
  assert.deepEqual(entries[0].params, [
    ['b', '2'],
    ['a', '1'],
    ['b', '3']
  ]);
});

test('entries: quoted semicolons, commas and angle brackets do not split', () => {
  const entries = parse.entries('<u>; title="a; b, c <d> e"; rel=next');
  assert.equal(entries[0].params[0][1], 'a; b, c <d> e');
});

test('entries: backslash escapes deliver the escaped character itself', () => {
  const entries = parse.entries('<u>; title="a\\;\\,\\<b\\\\c\\"d"; rel=next');
  assert.equal(entries[0].params[0][1], 'a;,<b\\c"d');
});

test('entries: commas and semicolons inside angle brackets are not separators', () => {
  const entries = parse.entries(
    '<http://a.test/x,y;z>; rel=next, <http://a.test/other>; rel=prev'
  );
  assert.equal(entries.length, 2);
  assert.equal(entries[0].target, 'http://a.test/x,y;z');
  assert.equal(entries[1].target, 'http://a.test/other');
});

test('entries: other ASCII whitespace works as separators', () => {
  const entries = parse.entries('<u>;\n\trel=next');
  assert.deepEqual(entries[0].rels, ['next']);
});

test('entries: an entry without rel still has an empty rels list', () => {
  const entries = parse.entries('<u>; title="t"');
  assert.deepEqual(entries[0].rels, []);
  assert.deepEqual(entries[0].params, [['title', 't']]);
});

// ---------------------------------------------------------------------------
// parse.entries: strict failures are atomic
// ---------------------------------------------------------------------------

test('entries: unclosed quote throws and produces no partial result', () => {
  const good = '<https://a.test/n1>; rel=next';
  const broken = good + ', <https://a.test/n2>; rel="next';
  assert.throws(() => parse.entries(broken), SyntaxError);
  // the first entry alone is valid, proving the failure is whole-input atomic
  assert.equal(parse.entries(good).length, 1);
  assert.throws(() => parse.nextPage(broken, RESPONSE_URL), SyntaxError);
});

test('entries: unclosed angle bracket throws', () => {
  assert.throws(() => parse.entries('<https://a.test/n; rel=next'), SyntaxError);
});

test('entries: invalid field syntax throws', () => {
  assert.throws(() => parse.entries('rel=next'), SyntaxError);          // no URI
  assert.throws(() => parse.entries('<u>; rel'), SyntaxError);          // no =
  assert.throws(() => parse.entries('<u>; rel='), SyntaxError);         // no value
  assert.throws(() => parse.entries('<u>;'), SyntaxError);              // dangling ;
  assert.throws(() => parse.entries('<u>,'), SyntaxError);              // dangling ,
  assert.throws(() => parse.entries('<u>; rel=next >'), SyntaxError);   // stray >
});

test('entries: empty URI throws', () => {
  assert.throws(() => parse.entries('<>; rel=next'), SyntaxError);
});

test('entries: duplicate rel throws', () => {
  assert.throws(() => parse.entries('<u>; rel=next; rel=prev'), SyntaxError);
});

test('entries: duplicate anchor throws', () => {
  assert.throws(() => parse.entries('<u>; anchor="x"; anchor="y"'), SyntaxError);
});

test('entries: non-string input throws a TypeError', () => {
  assert.throws(() => parse.entries(123), TypeError);
  assert.throws(() => parse.entries(['<u>', 42]), TypeError);
});

// ---------------------------------------------------------------------------
// parse.entries: UTF-8 byte budget
// ---------------------------------------------------------------------------

test('entries: byte budget covers field values and logical joining commas', () => {
  // fields: 'a' (1) + 'b' (1), one logical comma (1) => 3 bytes total
  const header = '<a>,<b>';
  assert.equal(parse.entries(header, { maxBytes: 3 }).length, 2);
  assert.throws(() => parse.entries(header, { maxBytes: 2 }), RangeError);
});

test('entries: commas joining separate header strings are counted too', () => {
  const headers = ['<a>', '<b>']; // 1 + 1 + joining comma = 3
  assert.equal(parse.entries(headers, { maxBytes: 3 }).length, 2);
  assert.throws(() => parse.entries(headers, { maxBytes: 2 }), RangeError);
});

test('entries: a single multibyte param straddling the cap (2-byte char)', () => {
  const mb = 'é'; // 2 UTF-8 bytes
  const n = 4;
  const header = '<u>; x="' + mb.repeat(n) + '"';
  // target 'u' = 1 byte, param value = 8 bytes, no commas => 9 bytes
  assert.equal(parse.entries(header, { maxBytes: 9 }).length, 1);
  assert.throws(() => parse.entries(header, { maxBytes: 8 }), RangeError);
});

test('entries: a single multibyte param straddling the cap (3-byte char)', () => {
  const mb = '中'; // 3 UTF-8 bytes
  const n = 4;
  const header = '<u>; x="' + mb.repeat(n) + '"';
  // target 'u' = 1 byte, param value = 12 bytes => 13 bytes
  assert.equal(parse.entries(header, { maxBytes: 13 }).length, 1);
  assert.throws(() => parse.entries(header, { maxBytes: 12 }), RangeError);
});

test('entries: default cap is 8192 UTF-8 bytes', () => {
  const atLimit = '<u>; x="' + 'a'.repeat(8191) + '"'; // 1 + 8191 = 8192
  assert.equal(parse.entries(atLimit).length, 1);

  const overLimit = '<u>; x="' + 'a'.repeat(8192) + '"'; // 8193
  assert.throws(() => parse.entries(overLimit), RangeError);
});

test('entries: bad maxBytes option throws', () => {
  assert.throws(() => parse.entries('<u>', { maxBytes: -1 }), RangeError);
  assert.throws(() => parse.entries('<u>', { maxBytes: 'nope' }), TypeError);
});

// ---------------------------------------------------------------------------
// parse.entries: parameter names are data, never object properties
// ---------------------------------------------------------------------------

test('entries: __proto__ and constructor stay plain data', () => {
  const entries = parse.entries('<u>; __proto__=x; constructor=y');
  assert.deepEqual(entries[0].params, [
    ['__proto__', 'x'],
    ['constructor', 'y']
  ]);
  assert.equal(Object.prototype.polluted, undefined);
  assert.equal(typeof {}.constructor, 'function'); // untouched
});

// ---------------------------------------------------------------------------
// parse.nextPage: the documented scenario
// ---------------------------------------------------------------------------

test('nextPage: quoted punctuation survives and off-context anchor is ignored', () => {
  const header =
    '<items?p=2&v=1&v=2#frag>; rel="Next prev"; title="x; y, <z>" , ' +
    '<https://b.test/wrong>; rel=next; anchor="?p=9"';

  assert.equal(
    parse.nextPage(header, RESPONSE_URL),
    'https://a.test/api/items?p=2&v=1&v=2'
  );
});

test('nextPage: an anchor matching the response makes a cross-origin next fatal', () => {
  const header =
    '<items?p=2>; rel=next, ' +
    '<https://b.test/wrong>; rel=next; anchor="?p=1"';

  // anchor "?p=1" resolves to the response URL (fragment-free) exactly
  assert.throws(() => parse.nextPage(header, RESPONSE_URL), /b\.test/);
});

test('nextPage: two distinct same-origin nexts are ambiguous', () => {
  const header =
    '<https://a.test/api/items?c=1>; rel=next, ' +
    '<https://a.test/api/items?c=2>; rel=next';
  assert.throws(() => parse.nextPage(header, RESPONSE_URL), /[Aa]mbiguous/);
});

test('nextPage: entries sharing one canonical URL count once', () => {
  const header =
    '<n?c=1#f1>; rel=next, <n?c=1#f2>; rel=next';
  assert.equal(
    parse.nextPage(header, 'https://a.test/api/items'),
    'https://a.test/api/n?c=1'
  );
});

test('nextPage: no candidate returns null', () => {
  assert.equal(parse.nextPage('<u>; rel=prev', RESPONSE_URL), null);
  assert.equal(
    parse.nextPage('<https://b.test/wrong>; rel=next; anchor="?p=9"', RESPONSE_URL),
    null
  );
});

test('nextPage: missing anchor binds the link to the response (relative target)', () => {
  assert.equal(
    parse.nextPage('<items?p=2>; rel=next', RESPONSE_URL),
    'https://a.test/api/items?p=2'
  );
});

test('nextPage: anchor fragments are stripped before comparing contexts', () => {
  const header =
    '<items?p=2>; rel=next; anchor="https://a.test/api/items?p=1#section"';
  assert.equal(
    parse.nextPage(header, RESPONSE_URL),
    'https://a.test/api/items?p=2'
  );
});

test('nextPage: query string participates in the context comparison', () => {
  // same path, different query -> different context, entry ignored
  const header = '<x>; rel=next; anchor="https://a.test/api/items?p=9"';
  assert.equal(parse.nextPage(header, RESPONSE_URL), null);
});

test('nextPage: non-http(s) targets are rejected', () => {
  assert.throws(
    () => parse.nextPage('<ftp://a.test/x>; rel=next', RESPONSE_URL),
    /http/
  );
});

test('nextPage: userinfo in the target is rejected', () => {
  assert.throws(
    () => parse.nextPage('<https://user:pass@a.test/x>; rel=next', RESPONSE_URL),
    /username or password/
  );
  assert.throws(
    () => parse.nextPage('<https://user@a.test/x>; rel=next', RESPONSE_URL),
    /username or password/
  );
});

test('nextPage: cross-origin http(s) targets are rejected', () => {
  assert.throws(
    () => parse.nextPage('<https://b.test/x>; rel=next', RESPONSE_URL),
    /origin/
  );
  assert.throws(
    () => parse.nextPage('<https://a.test:8443/x>; rel=next', RESPONSE_URL),
    /origin/
  );
  assert.throws(
    () => parse.nextPage('<http://a.test/x>; rel=next', RESPONSE_URL),
    /origin/
  );
});

test('nextPage: a target normalizing back to the response is rejected', () => {
  assert.throws(
    () => parse.nextPage('<https://a.test/api/items?p=1#frag>; rel=next', RESPONSE_URL),
    /itself/
  );
});

test('nextPage: fragments are dropped, query order and duplicates preserved', () => {
  assert.equal(
    parse.nextPage('<items?p=2&v=1&v=2#frag>; rel=next', RESPONSE_URL),
    'https://a.test/api/items?p=2&v=1&v=2'
  );
});

test('nextPage: next may appear among several rels', () => {
  assert.equal(
    parse.nextPage('<items?p=2>; rel="prev next last"', RESPONSE_URL),
    'https://a.test/api/items?p=2'
  );
});

test('nextPage: options (maxBytes) are forwarded to the strict parser', () => {
  const header = '<items?p=2>; rel=next; data="' + 'a'.repeat(100) + '"';
  assert.throws(
    () => parse.nextPage(header, RESPONSE_URL, { maxBytes: 10 }),
    RangeError
  );
});

test('public shape: default callable plus two named methods', () => {
  assert.equal(typeof parse, 'function');
  assert.equal(typeof parse.entries, 'function');
  assert.equal(typeof parse.nextPage, 'function');
});
