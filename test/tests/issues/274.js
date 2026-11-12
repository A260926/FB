const { parse } = require('@test/test-target');

describe('issue 274', function () {
	it('removeWhitespace + outerHTML dont remove meaningless white-spaces inside start and end tags', function () {
		const html = `<!DOCTYPE html
><html                     lang="en"
><meta charset="UTF-8"
><title>test</title

><p>test</p

></html>`;
		const root = parse(html);
		const el = root.querySelector('html').removeWhitespace();
		el.outerHTML.should.eql('<html lang="en"><meta charset="UTF-8"><title>test</title><p>test</p></html>');
	});
});
