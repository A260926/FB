'use strict';

const assert = require('node:assert/strict');
const { test } = require('node:test');
const { parse } = require('../.build/index.cjs');

test('composed-structure', async (t) => {
	await t.test('expands the open template and keeps exact node identities', () => {
		const html =
			'<product-card><template shadowrootmode="open"><section><slot name="price">Unknown</slot></section></template><b slot="price" id="cost">12 &amp; 34</b><i id="unassigned">private</i></product-card>';
		const root = parse(html);
		const host = root.firstChild;
		const price = host.querySelector('#cost');

		// The assigned <b> is selectable through the template's section, and
		// the result is the very same object that lives in the physical tree.
		const throughSection = host.querySelectorAllComposed('section > b');
		assert.equal(throughSection.length, 1);
		assert.equal(throughSection[0], price);
		assert.equal(throughSection[0].text, '12 & 34');

		assert.deepEqual(host.querySelectorAllComposed('b').map((node) => node.id), ['cost']);
		// Slots, templates and unassigned light DOM take no position in the view.
		assert.equal(host.querySelectorAllComposed('template').length, 0);
		assert.equal(host.querySelectorAllComposed('slot').length, 0);
		assert.equal(host.querySelectorAllComposed('#unassigned').length, 0);
		assert.equal(host.querySelectorAllComposed('i').length, 0);
		// Like querySelectorAll, the calling element itself is never returned.
		assert.equal(host.querySelectorAllComposed('product-card').length, 0);

		const structured = host.structuredTextComposed;
		assert.match(structured, /12 & 34/);
		assert.doesNotMatch(structured, /Unknown/);
		assert.doesNotMatch(structured, /private/);
		// Two consecutive reads produce identical output.
		assert.equal(host.structuredTextComposed, structured);

		// The physical tree and serialization stay untouched.
		assert.equal(host.querySelector('section').parentNode.rawTagName, 'template');
		assert.equal(host.querySelector('slot').text, 'Unknown');
		assert.match(host.structuredText, /Unknown/);
		assert.match(host.structuredText, /private/);
		assert.equal(price.parentNode, host);
		assert.equal(root.toString(), html);
	});

	await t.test('assigns named and default slots, with first-slot-wins and fallback', () => {
		const html =
			'<card><template shadowrootmode="open"><div><slot name="title">fallback-title</slot><p><slot>default-fallback</slot></p><slot name="title">dup-fallback</slot></div></template><h2 slot="title">Hi</h2><span>def</span></card>';
		const card = parse(html).firstChild;

		assert.equal(card.querySelectorAllComposed('div > h2').length, 1);
		assert.deepEqual(card.querySelectorAllComposed('p > *').map((node) => node.rawTagName), ['span']);

		const text = card.structuredTextComposed;
		assert.ok(text.includes('Hi'));
		assert.ok(text.includes('def'));
		assert.ok(!text.includes('fallback-title'));
		assert.ok(!text.includes('default-fallback'));
		// A repeated same-name slot is not the first slot of that name, so it
		// keeps rendering its own fallback content.
		assert.ok(text.includes('dup-fallback'), text);
	});

	await t.test('whitespace-only assignment still suppresses fallback content', () => {
		const assigned = parse(
			'<card><template shadowrootmode="open"><p><slot>FB</slot></p></template>   </card>'
		).firstChild;
		assert.doesNotMatch(assigned.structuredTextComposed, /FB/);

		const unassigned = parse(
			'<card><template shadowrootmode="open"><p><slot>FB</slot></p></template><b slot="other">x</b></card>'
		).firstChild;
		assert.match(unassigned.structuredTextComposed, /FB/);
		assert.equal(unassigned.querySelectorAllComposed('b').length, 0);
	});

	await t.test('expands nested hosts in two separate assignment steps', () => {
		const html =
			'<outer><template shadowrootmode="open"><section><slot name="inner"></slot></section></template><inner-card slot="inner"><template shadowrootmode="open"><h1><slot name="t">nh</slot></h1></template><b slot="t" id="deep">deep</b></inner-card></outer>';
		const outer = parse(html).firstChild;
		const deep = outer.querySelector('#deep');

		const matches = outer.querySelectorAllComposed('section inner-card h1 > b');
		assert.equal(matches.length, 1);
		assert.equal(matches[0], deep);
		assert.equal(outer.querySelectorAllComposed('h1').length, 1);

		// A physical node must not show up twice through nested assignment.
		const all = outer.querySelectorAllComposed('*');
		assert.equal(new Set(all).size, all.length);
		// The physical parent chain is never rewritten for the view.
		assert.equal(deep.parentNode.rawTagName, 'inner-card');
	});

	await t.test('computes sibling, positional, :contains() and :empty relations from the view', () => {
		const html =
			'<x><template shadowrootmode="open"><ul><slot></slot></ul><ul><slot name="x"></slot></ul></template><li id="a">one</li><li id="b">two</li></x>';
		const x = parse(html).firstChild;

		assert.deepEqual(x.querySelectorAllComposed('li + li').map((node) => node.id), ['b']);
		assert.deepEqual(x.querySelectorAllComposed('li:first-child').map((node) => node.id), ['a']);
		assert.deepEqual(x.querySelectorAllComposed('li:nth-child(2)').map((node) => node.id), ['b']);
		assert.deepEqual(x.querySelectorAllComposed('li:contains("two")').map((node) => node.id), ['b']);
		// The second ul's named slot receives nothing, so it is empty in the
		// view even though its physical child slot element still exists.
		const emptyUls = x.querySelectorAllComposed('ul:empty');
		assert.equal(emptyUls.length, 1);
		assert.equal(x.querySelector('ul:empty'), null);
		assert.equal(x.querySelectorAllComposed('li:empty').length, 0);
	});

	await t.test('supports compound selectors, :scope and leaves closed/inert templates alone', () => {
		const compound = parse(
			'<x><template shadowrootmode="open"><section><slot name="a"></slot></section></template><div class="a" slot="a"><span id="inner">yo</span></div></x>'
		).firstChild;
		assert.equal(compound.querySelectorAllComposed('section > div.a:not(.b):has(span#inner)').length, 1);
		assert.equal(compound.querySelectorAllComposed('section > div:has(> span)').length, 1);
		assert.equal(compound.querySelectorAllComposed('div:not([slot])').length, 0);

		const scoped = parse(
			'<x><template shadowrootmode="open"><div><slot></slot></div></template><b id="z">z</b></x>'
		).firstChild;
		assert.deepEqual(scoped.querySelectorAllComposed(':scope > div').map((node) => node.rawTagName), ['div']);
		assert.deepEqual(scoped.querySelectorAllComposed(':scope b').map((node) => node.id), ['z']);
		assert.equal(scoped.querySelectorAllComposed(':scope').length, 0);

		const closed = parse(
			'<x><template shadowrootmode="closed"><p>hidden</p></template><b>shown</b></x>'
		).firstChild;
		assert.equal(closed.querySelectorAllComposed('p').length, 0);
		assert.equal(closed.querySelectorAllComposed('b').length, 1);

		const inert = parse('<x><template><p>inert</p></template><b>ok</b></x>').firstChild;
		assert.equal(inert.querySelectorAllComposed('p').length, 0);
		assert.deepEqual(inert.querySelectorAllComposed('b').map((node) => node.text), ['ok']);

		const twoTemplates = parse(
			'<x><template shadowrootmode="open"><p><slot></slot></p></template><template shadowrootmode="open"><div>second</div></template><b id="b1">v</b></x>'
		).firstChild;
		assert.deepEqual(twoTemplates.querySelectorAllComposed('*').map((node) => node.rawTagName), ['p', 'b']);
		assert.equal(twoTemplates.querySelectorAllComposed('div').length, 0);
	});

	await t.test('keeps structured-text block boundaries and entity decoding on the composed tree', () => {
		const card = parse(
			'<x><template shadowrootmode="open"><div><h2><slot name="h"></slot></h2><p><slot></slot></p></div></template><b slot="h">Title</b><i>body text</i></x>'
		).firstChild;
		assert.equal(card.structuredTextComposed, 'Title\nbody text');

		const entities = parse(
			'<x><template shadowrootmode="open"><p><slot></slot></p></template>A &amp; B</x>'
		).firstChild;
		assert.equal(entities.structuredTextComposed, 'A & B');
	});
});

test('composed-requery', async (t) => {
	await t.test('reacts to slot attribute changes without losing node identity', () => {
		const html =
			'<x><template shadowrootmode="open"><p><slot name="s">fb</slot></p></template><b slot="s" id="one">1</b></x>';
		const x = parse(html).firstChild;
		const one = x.querySelector('#one');

		assert.equal(x.querySelectorAllComposed('p > b').length, 1);
		const snapshot = x.querySelectorAllComposed('p > b')[0];

		one.removeAttribute('slot');
		assert.equal(x.querySelectorAllComposed('p > b').length, 0);
		assert.match(x.structuredTextComposed, /fb/);
		// The previously returned object is the same physical node and the old
		// interfaces still show the original structure.
		assert.equal(snapshot, one);
		assert.equal(one.text, '1');
		assert.equal(x.toString(), html.replace(' slot="s"', ''));

		one.setAttribute('slot', 's');
		const again = x.querySelectorAllComposed('p > b');
		assert.equal(again.length, 1);
		assert.equal(again[0], one);
		assert.doesNotMatch(x.structuredTextComposed, /fb/);
	});

	await t.test('reflects replaced template content on the next call', () => {
		const html =
			'<x><template shadowrootmode="open"><p><slot name="s">fb</slot></p></template><b slot="s" id="bn">v</b></x>';
		const x = parse(html).firstChild;
		assert.equal(x.querySelectorAllComposed('p > b').length, 1);

		x.querySelector('template').innerHTML = '<section><slot name="s"></slot></section>';
		assert.equal(x.querySelectorAllComposed('p').length, 0);
		const moved = x.querySelectorAllComposed('section > b');
		assert.equal(moved.length, 1);
		assert.equal(moved[0].id, 'bn');
		assert.equal(x.structuredTextComposed, 'v');
		// Templates themselves still never appear in the view, while the
		// physical serialization shows the replacement.
		assert.equal(x.querySelectorAllComposed('template').length, 0);
		assert.match(x.toString(), /<section>/);
	});

	await t.test('follows light DOM moved between sibling hosts', () => {
		const html =
			'<wrap><a-card><template shadowrootmode="open"><span><slot></slot></span></template><em id="m">moving</em></a-card><a-card><template shadowrootmode="open"><span><slot></slot></span></template></a-card></wrap>';
		const wrap = parse(html).firstChild;
		const cards = wrap.querySelectorAllComposed('a-card');
		assert.equal(cards.length, 2);
		const moving = wrap.querySelector('#m');

		assert.equal(cards[0].querySelectorAllComposed('span > em').length, 1);
		assert.equal(cards[1].querySelectorAllComposed('span > em').length, 0);

		cards[1].appendChild(moving.remove());
		assert.equal(cards[0].querySelectorAllComposed('span > em').length, 0);
		assert.equal(cards[1].querySelectorAllComposed('span > em')[0], moving);
		assert.equal(moving.parentNode, cards[1]);
	});

	await t.test('gives stable repeated reads and keeps the old APIs on the physical tree', () => {
		const html =
			'<product-card><template shadowrootmode="open"><section><slot name="price">Unknown</slot></section></template><b slot="price" id="cost">12 &amp; 34</b><i id="unassigned">private</i></product-card>';
		const host = parse(html).firstChild;
		const first = host.querySelectorAllComposed('section > b');
		const second = host.querySelectorAllComposed('section > b');
		assert.equal(first[0], second[0]);
		assert.equal(host.structuredTextComposed, host.structuredTextComposed);

		// Old query, text and serialization interfaces keep their physical-tree behavior.
		assert.equal(host.querySelector('slot').text, 'Unknown');
		assert.match(host.structuredText, /private/);
		assert.equal(host.toString(), html);
	});
});
