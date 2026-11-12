const { parse } = require('@test/test-target');

describe('issue 240', function () {
	it(`attribute should not changed`, function () {
		const html = "<div unchanged='[\npreserve newline\n]'></div>";
		const root = parse(html);
		const div = root.firstChild
		div.toString().should.eql(html);

		div.setAttribute("append", "newAttribute");
		div.toString().should.eql('<div unchanged="[\npreserve newline\n]" append="newAttribute"></div>');
	});
});
