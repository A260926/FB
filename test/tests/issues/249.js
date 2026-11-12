const { parse } = require('@test/test-target');

describe('issue 249', function () {
	it('br in innertext should turn into \\n', function () {
		const html = `<div>Hello<br>World</div>`;
		const root = parse(html);
		const div = root.querySelector('div');
		div.innerText.should.eql(`Hello
World`);
	});
});
