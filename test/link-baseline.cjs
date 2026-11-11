'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const parse = require('..');

test('ordinary Link retains the original rel-keyed API', () => {
    const parsed = parse('<https://a.test/api/items?p=2>; rel="next prev", <https://a.test/api/items?p=9>; rel=last');
    assert.equal(parsed.next.url, 'https://a.test/api/items?p=2');
    assert.equal(parsed.next.p, '2');
    assert.equal(parsed.prev.rel, 'prev');
    assert.equal(parsed.last.p, '9');
    assert.equal(parse(''), null);
});
