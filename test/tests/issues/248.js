const { valid } = require('@test/test-target');

describe('issue 248', function () {
	it('void tag', function () {
		const voidTags = ['x-tag'];
		const html = `<div><x-tag></div>`;
		// => false. Expected to be true
		valid(html, {
			voidTag: {
				tags: voidTags,
			}
		}).should.eql(true);
	});
	it('selfclosed tag', function () {
		const voidTags = ['x-tag'];
		const html = `<div><x-tag /></div>`;
		// => false. Expected to be true
		valid(html, {
			voidTag: {
				tags: voidTags,
				closingSlash: true
			}
		}).should.eql(true);
	});
});
