const { parse } = require('@test/test-target');

describe('issue 242', function () {
	it(`a.rawAttrs returns 'href="/" rel="home"' but a.getAttribute("href') returns undefined`, function () {
		const html = `<div><a href="/" rel="home">Git Hub</a></div>`;
		const root = parse(html);
		const links = root.querySelectorAll("a");

		const [a] = links;
		a.rawAttrs.should.eql('href="/" rel="home"');
		a.getAttribute('href').should.eql('/');
	});
	it(`get code`, function () {
		const html = `<pre>
  <code>test</code>
</pre>`;
		const root = parse(html, {
			blockTextElements: {
			}
		});
		const list = root.getElementsByTagName("code");
		const [code] = list;
		code.text.should.eql('test');
	});
	it(`do not block text element`, function () {
		const htmlString = "sample <b><strong>text</strong> inside tags</b> <script>text inside script</script>"

		const root = parse(htmlString, {
			blockTextElements: {
				script: false
			}
		});

		root.text.should.eql('sample text inside tags ');
	});
	it(`block text element`, function () {
		const htmlString = "sample <b><strong>text</strong> inside tags</b> <script>text inside script</script>"

		const root = parse(htmlString, {
			blockTextElements: {
				script: true
			}
		});

		root.text.should.eql('sample text inside tags text inside script');
	});
});
