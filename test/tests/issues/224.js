const fs = require('fs');
const { parse } = require('@test/test-target');

describe('issue 224', function () {
	it('query', function () {
		const html = fs.readFileSync(__dirname + '/../../assets/html/melon.html', 'utf-8');
		const root = parse(html);
		const el = root.querySelector(".band:nth-of-type(3) .col-md-4:nth-of-type(2) h2")
		el.tagName.should.eql('H2');
	});
});
