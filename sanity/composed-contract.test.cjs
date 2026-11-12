#!/usr/bin/env node
/**
 * Contract tests for the composed (declarative shadow DOM expanded) view:
 * HTMLElement#querySelectorAllComposed and HTMLElement#structuredTextComposed.
 *
 * The view expands `<template shadowrootmode="open">` hosts, assigns light
 * DOM to slots and hides inert templates, fallback content that got replaced
 * and unassigned light DOM. Returned nodes are always the very objects of
 * the physical tree, and the classic APIs keep seeing that tree unchanged.
 */
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { parse } = require('node-html-parser');

// The product-card example from the task: the template is
// `section > slot[name=price]`, the light DOM brings the real price `b` and
// an unassigned private field.
const CARD_HTML =
	'<product-card><template shadowrootmode="open"><section>Price: <slot name="price"><b>Unknown</b></slot></section></template><b slot="price">$12.5</b><span class="private">warehouse-7</span></product-card>';

test('composed-structure', async (t) => {
	await t.test('named slot: assigned light DOM takes the slot position', () => {
		const root = parse(CARD_HTML);
		const card = root.querySelector('product-card');
		// Physical objects, looked up through the classic APIs.
		const priceB = root.querySelector('product-card > b[slot="price"]');
		const fallbackB = root.querySelector('template b');

		// The price b is matched by `section > b`: the slot is transparent and
		// the physical template is not part of the relation chain.
		const matches = card.querySelectorAllComposed('section > b');
		assert.equal(matches.length, 1);
		assert.equal(matches[0], priceB); // the exact object from the physical tree
		assert.notEqual(matches[0], fallbackB);
		assert.equal(matches[0].text, '$12.5'); // the exact text
		assert.equal(card.querySelectorAllComposed('section > b:first-child').length, 1);

		// The whole chain matches across the host boundary...
		assert.equal(root.querySelectorAllComposed('product-card > section > b').length, 1);
		// ...but the b is no longer a composed child of the host itself.
		assert.equal(root.querySelectorAllComposed('product-card > b').length, 0);
		assert.equal(root.querySelectorAll('product-card > b').length, 1); // physically it is

		// slot and template occupy no position in the view.
		assert.equal(card.querySelectorAllComposed('template').length, 0);
		assert.equal(card.querySelectorAllComposed('slot').length, 0);
		assert.equal(card.querySelectorAllComposed('template b').length, 0);
		assert.equal(card.querySelectorAllComposed('section > slot').length, 0);

		// The replaced fallback copy and the unassigned private field drop out.
		assert.deepEqual(
			card.querySelectorAllComposed('b').map((b) => b.text),
			['$12.5']
		);
		assert.equal(card.querySelectorAllComposed('.private').length, 0);
		assert.equal(card.structuredTextComposed, 'Price: $12.5');
	});

	await t.test('default slot receives unslotted elements and text', () => {
		const root = parse('<card><template shadowrootmode="open"><div class="body"><slot>empty</slot></div></template><em>main</em> <u>sec</u></card>');
		const card = root.querySelector('card');
		const em = root.querySelector('card > em');
		const u = root.querySelector('card > u');

		assert.deepEqual(card.querySelectorAllComposed('div.body > *'), [em, u]);
		assert.equal(card.querySelectorAllComposed('em + u').length, 1);
		// The default slot's own fallback copy is replaced.
		assert.equal(card.structuredTextComposed, 'main sec');
		assert.ok(!card.structuredTextComposed.includes('empty'));
	});

	await t.test('unassigned slot shows its fallback, duplicates keep theirs', () => {
		const root = parse(
			'<card><template shadowrootmode="open"><footer><slot name="foot">first-fallback</slot><slot name="foot">second-fallback</slot><slot name="missing">nothing-assigned</slot></footer></template><i slot="foot">F</i></card>'
		);
		const card = root.querySelector('card');
		const assigned = root.querySelector('card > i');

		// Only the first `foot` slot (template tree order) receives the
		// assignment; the duplicate and the unclaimed slot keep their fallback.
		assert.deepEqual(card.querySelectorAllComposed('footer > *'), [assigned]);
		assert.equal(card.structuredTextComposed, 'Fsecond-fallbacknothing-assigned');
		assert.ok(!card.structuredTextComposed.includes('first-fallback'));
	});

	await t.test('whitespace-only assignment still suppresses the fallback', () => {
		// The host's only light DOM is a whitespace text node.
		const root = parse('<card><template shadowrootmode="open"><div><slot>fallback</slot></div></template> </card>');
		const card = root.querySelector('card');
		assert.equal(card.structuredTextComposed, '');
		// The whitespace text occupies the slot, so the div is not empty...
		assert.equal(card.querySelectorAllComposed('div:empty').length, 0);
		assert.ok(!card.structuredTextComposed.includes('fallback'));

		// ...while with no light DOM at all the fallback shows up.
		const bare = parse('<card><template shadowrootmode="open"><div><slot>fallback</slot></div></template></card>').querySelector('card');
		assert.equal(bare.structuredTextComposed, 'fallback');
		assert.equal(bare.querySelectorAllComposed('div:empty').length, 0);
	});

	await t.test('nested hosts expand by their own template and light DOM', () => {
		const root = parse(
			'<outer-card><template shadowrootmode="open"><section><slot name="head"></slot><inner-card><template shadowrootmode="open"><div class="inner"><slot></slot></div></template><span class="il">inner text</span></inner-card></section></template><h1 slot="head">Title</h1></outer-card>'
		);
		const outer = root.querySelector('outer-card');
		const h1 = root.querySelector('outer-card > h1');
		const innerCard = root.querySelector('inner-card');
		const innerLight = root.querySelector('span.il');

		// The outer template places the assigned h1 next to the inner host.
		assert.deepEqual(outer.querySelectorAllComposed('section > *'), [h1, innerCard]);
		assert.equal(outer.querySelectorAllComposed('h1 + inner-card').length, 1);

		// The inner host expands by its own template and light DOM: its light
		// DOM shows up exactly once, inside the inner template's structure.
		assert.deepEqual(outer.querySelectorAllComposed('.il'), [innerLight]);
		assert.deepEqual(outer.querySelectorAllComposed('div.inner > .il'), [innerLight]);
		assert.equal(outer.querySelectorAllComposed('span').length, 1);
		assert.equal(outer.querySelectorAllComposed('inner-card > div.inner').length, 1);

		assert.equal(outer.structuredTextComposed, 'Title\ninner text');
	});

	await t.test('inert templates stay out of the view', () => {
		const root = parse(
			'<div id="e"><template><span>plain</span></template><template shadowrootmode="closed"><span>closed</span></template><template shadowrootmode="open"><p>shown</p></template><template shadowrootmode="open"><p>second</p></template></div>'
		);
		const host = root.querySelector('#e');
		// Only the first open template is the host's template; every other
		// template (plain, closed, or later) is inert content.
		assert.deepEqual(
			host.querySelectorAllComposed('p').map((p) => p.text),
			['shown']
		);
		assert.equal(host.querySelectorAllComposed('span').length, 0);
		assert.equal(host.querySelectorAllComposed('template').length, 0);
		assert.equal(host.structuredTextComposed, 'shown');
	});

	await t.test('css combinators and pseudos follow the composed view', () => {
		const root = parse(
			'<card><template shadowrootmode="open"><section><p class="line">Price: <slot name="price"><b>Unknown</b></slot></p><div class="filled"><slot name="a"></slot></div><div class="hollow"><slot name="b"></slot></div></section></template><b slot="price">$9</b><span slot="a">x</span></card>'
		);
		const card = root.querySelector('card');
		const priceB = root.querySelector('card > b');

		// Compound selector and combinators across the transparent slot.
		assert.equal(card.querySelectorAllComposed('section > p.line').length, 1);
		assert.deepEqual(card.querySelectorAllComposed('p.line > b'), [priceB]);
		assert.deepEqual(card.querySelectorAllComposed('section b'), [priceB]);

		// :contains() reads the composed text, not the physical one.
		assert.equal(card.querySelectorAllComposed('p:contains("$9")').length, 1);
		assert.equal(card.querySelectorAllComposed('p:contains("Unknown")').length, 0);
		assert.equal(card.querySelectorAllComposed('section:contains("x")').length, 1);

		// :empty judges the composed children: a slot with neither assignment
		// nor fallback leaves the div empty, a filled slot does not.
		assert.equal(card.querySelectorAllComposed('div.hollow:empty').length, 1);
		assert.equal(card.querySelectorAllComposed('div.filled:empty').length, 0);
		assert.equal(card.querySelectorAll('div.hollow:empty').length, 0); // physically the slot is there

		// Positional relations count the expanded view only.
		const positioned = parse(
			'<card><template shadowrootmode="open"><section><slot name="price">?</slot><span class="after">tail</span></section></template><b slot="price">$9</b></card>'
		).querySelector('card');
		assert.equal(positioned.querySelectorAllComposed('section > :first-child').length, 1);
		assert.equal(positioned.querySelectorAllComposed('section > :first-child')[0].text, '$9');
		assert.equal(positioned.querySelectorAllComposed('b + span.after').length, 1);
		assert.deepEqual(
			positioned.querySelectorAllComposed('section > :nth-child(2)').map((el) => el.rawTagName),
			['span']
		);

		// :scope is the element the query starts from, and is never returned.
		assert.equal(card.querySelectorAllComposed(':scope > section').length, 1);
		assert.equal(card.querySelectorAllComposed(':scope > template').length, 0);
		assert.equal(card.querySelectorAllComposed(':scope').length, 0);
		assert.equal(root.querySelectorAllComposed(':scope > card').length, 1);
	});

	await t.test('structuredTextComposed keeps blocks, entities and whitespace rules', () => {
		const root = parse('<card><template shadowrootmode="open"><div><slot name="t"></slot></div><p>tail &amp; end</p></template><span slot="t">A &amp; B</span></card>');
		const card = root.querySelector('card');
		// Same block boundaries, entity decoding and whitespace handling as
		// structuredText, but over the expanded view.
		assert.equal(card.structuredTextComposed, 'A & B\ntail & end');
		// Two consecutive reads agree.
		assert.equal(card.structuredTextComposed, card.structuredTextComposed);
		// The physical structured text still walks the physical tree.
		assert.equal(card.structuredText, 'tail & end\nA & B');
	});

	await t.test('physical interfaces keep seeing the physical tree', () => {
		const root = parse(CARD_HTML);
		const card = root.querySelector('product-card');
		assert.deepEqual(
			card.querySelectorAll('b').map((b) => b.text),
			['Unknown', '$12.5']
		);
		assert.equal(card.querySelectorAll('.private').length, 1);
		assert.equal(card.structuredText, 'Price: Unknown\n$12.5warehouse-7');
		assert.equal(card.outerHTML, CARD_HTML);
	});
});

test('composed-requery', async (t) => {
	const R_HTML =
		'<card-r><template shadowrootmode="open"><section><slot name="price"><b>Unknown</b></slot></section></template><b slot="price">$1</b><span class="private">secret</span></card-r>';

	await t.test('slot attribute changes are reflected on the next read', () => {
		const root = parse(R_HTML);
		const card = root.querySelector('card-r');
		const priceB = root.querySelector('card-r > b');
		const fallbackB = root.querySelector('template b');

		assert.deepEqual(card.querySelectorAllComposed('section > b'), [priceB]);
		assert.equal(card.structuredTextComposed, '$1');

		// Detaching the price from its slot makes it unassigned: the fallback returns.
		priceB.removeAttribute('slot');
		assert.deepEqual(card.querySelectorAllComposed('section > b'), [fallbackB]);
		assert.equal(card.structuredTextComposed, 'Unknown');

		// Assigning the private field to the slot brings it into the view.
		const span = root.querySelector('span.private');
		span.setAttribute('slot', 'price');
		assert.deepEqual(card.querySelectorAllComposed('section > span.private'), [span]);
		assert.equal(card.querySelectorAllComposed('section > b').length, 0);

		priceB.setAttribute('slot', 'price');
		assert.deepEqual(card.querySelectorAllComposed('section > b'), [priceB]);
	});

	await t.test('moving light DOM is reflected on the next read', () => {
		const root = parse(R_HTML);
		const card = root.querySelector('card-r');
		const priceB = root.querySelector('card-r > b');

		card.removeChild(priceB);
		assert.deepEqual(
			card.querySelectorAllComposed('section > b').map((b) => b.text),
			['Unknown']
		);

		card.appendChild(priceB);
		assert.deepEqual(card.querySelectorAllComposed('section > b'), [priceB]);

		// Several nodes assigned to one slot follow light DOM order.
		const extra = parse('<b slot="price">$2</b>').firstChild;
		card.appendChild(extra);
		assert.deepEqual(
			card.querySelectorAllComposed('section > b').map((b) => b.text),
			['$1', '$2']
		);
		card.removeChild(priceB);
		card.appendChild(priceB); // moves to the end of the light DOM
		assert.deepEqual(
			card.querySelectorAllComposed('section > b').map((b) => b.text),
			['$2', '$1']
		);
	});

	await t.test('replacing template content is reflected on the next read', () => {
		const root = parse(R_HTML);
		const card = root.querySelector('card-r');
		const priceB = root.querySelector('card-r > b');
		const template = card.childNodes[0];

		template.innerHTML = '<div><slot name="price"></slot></div>';
		assert.deepEqual(card.querySelectorAllComposed('div > b'), [priceB]);
		assert.equal(card.querySelectorAllComposed('section').length, 0);
		assert.equal(card.structuredTextComposed, '$1');
	});

	await t.test('returned nodes keep their identity, legacy APIs see the physical tree', () => {
		const root = parse(R_HTML);
		const card = root.querySelector('card-r');
		const priceB = root.querySelector('card-r > b');
		const fallbackB = root.querySelector('template b');

		const before = card.querySelectorAllComposed('section > b')[0];
		assert.equal(before, priceB);

		priceB.removeAttribute('slot');
		assert.equal(card.querySelectorAllComposed('section > b')[0], fallbackB);
		assert.equal(card.structuredTextComposed, 'Unknown');
		// Reads stay consistent after the mutation as well.
		assert.equal(card.structuredTextComposed, card.structuredTextComposed);

		priceB.setAttribute('slot', 'price');
		assert.equal(card.querySelectorAllComposed('section > b')[0], before);

		// The physical tree is untouched by the composed reads and mutations
		// of the view: classic queries, text and serialization are unchanged.
		assert.deepEqual(
			card.querySelectorAll('b').map((b) => b.text),
			['Unknown', '$1']
		);
		assert.equal(card.childNodes[0].rawTagName, 'template');
		assert.equal(card.outerHTML, R_HTML);
	});
});
