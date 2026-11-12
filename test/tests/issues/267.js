const { parse } = require('@test/test-target');

describe('issue 267', function () {
	it('Incorrect Handling of Empty Class Attributes in SVG Parsing', function () {
		const html = `<polygon points="-235.18 1571.95 1014.73 1284.4 1083.46 1590.1 -166.45 1877.65 -235.18 1571.95" fill="#ff8200" class="" design-color="primary"></polygon> `;
		const root = parse(html);
		root.toString().should.eql(html);
	});
});
