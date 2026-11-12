const { parse } = require('@test/test-target');

describe('issue 269 270', function () {
	it('node.rawTagName', function () {
		const root = parse('<div><!--this is comment here -->foo</div>', { comment: true });
		const div = root.childNodes[0];
		div.rawTagName.should.eql('div');
		div.childNodes.length.should.eql(2);
		const comment = div.childNodes[0];
		comment.rawTagName.should.eql('!--');
		const text = div.childNodes[1];
		text.rawTagName.should.eql('');
	});
	it('querySelector for comment nodes', function () {
		const root = parse(`<html>
  <body>
    <h1>TEST</h1>
    <!-- Some comment here. -->
  </body>
</html>`, { comment: true });
		const div = root.childNodes[0];
		const comment = div.querySelector('!--');
		comment.text.should.eql(' Some comment here. ');
	});
});
