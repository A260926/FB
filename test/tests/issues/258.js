const { parse } = require('@test/test-target');

describe('issue 258', function () {
	it('removeAttribute makes boolean attributes render incorrectly.', function () {
		const inputEl = parse('<input>').firstChild

		inputEl.setAttribute('checked', '');
		inputEl.setAttribute('a', '');
		inputEl.toString().should.eql('<input checked a>');

		inputEl.removeAttribute('a');
		inputEl.toString().should.eql('<input checked>');
	});
});
