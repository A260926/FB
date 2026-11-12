const { parse } = require('@test/test-target');
const fs = require('fs');

describe('issue 260', function () {
	it('100% cpu while parsing document', function () {
		const html = fs.readFileSync(__dirname + '/../../assets/html/a1supplements.com.html', 'utf-8');
		const root = parse(html, {
			parseNoneClosedTags: true
		});
		// const root = parse(html, {
		// 	parseNoneClosedTags: false,
		// 	fixNestedATags: false,
		// 	blockTextElements: {
		// 		'div': true,
		// 		'p': true,
		// 		'pre': true
		// 	}
		// });
		const testNode = root.querySelector('#sca-fg-today-offer-widget');
		testNode.toString().should.eql('<button id="sca-fg-today-offer-widget"></button>');
	});
});
