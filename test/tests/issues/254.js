const { parse } = require('@test/test-target');

describe('issue 254', function () {
	it('abbr in innertext should not turn into \\n', function () {
		const html = `<div>Hello <abbr>World</abbr></div>`;
		const root = parse(html);
		const div = root.querySelector('div');
		div.innerText.should.eql(`Hello World`);
	});
});
