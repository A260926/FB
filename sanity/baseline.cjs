#!/usr/bin/env node
/**
 * Baseline: what the classic (physical-tree) APIs of node-html-parser see
 * when a product page writes its card internals as declarative shadow DOM.
 *
 * In the physical tree the template's fallback copy ("Unknown"), the real
 * price and the unassigned "private" field are all walked together, and the
 * template itself is part of the queryable structure. The composed view
 * (querySelectorAllComposed / structuredTextComposed) exists precisely to
 * separate these; this script pins down the classic behaviour so the new
 * APIs can never change it. It exits non-zero if the behaviour drifts.
 */
const assert = require('node:assert/strict');
const { parse } = require('node-html-parser');

const HTML =
	'<product-card><template shadowrootmode="open"><section>Price: <slot name="price"><b>Unknown</b></slot></section></template><b slot="price">$12.5</b><span class="private">warehouse-7</span></product-card>';

const root = parse(HTML);
const card = root.querySelector('product-card');

// Fallback copy, real price and unassigned private field are all in the tree.
assert.deepEqual(
	card.querySelectorAll('b').map((b) => b.text),
	['Unknown', '$12.5']
);
assert.equal(card.querySelectorAll('.private').length, 1);
assert.equal(card.querySelectorAll('template slot').length, 1);

// The classic structured text walks everything, template content included.
assert.equal(card.structuredText, 'Price: Unknown\n$12.5warehouse-7');

// Serialization keeps the declarative template untouched.
assert.equal(card.outerHTML, HTML);

console.log('baseline: physical-tree behaviour unchanged');
console.log('  querySelectorAll("b")  ->', JSON.stringify(card.querySelectorAll('b').map((b) => b.text)));
console.log('  structuredText         ->', JSON.stringify(card.structuredText));
