const { parse } = require('@test/test-target');

describe('issue 239', function () {
	it('query', function () {
		const root = parse('<ul id="list"><li>Hello World</li></ul>');
		root.querySelector('#list').toString().should.eql('<ul id="list"><li>Hello World</li></ul>');
		root.querySelector('li').toString().should.eql('<li>Hello World</li>');
	});
});
