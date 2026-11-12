const { parse } = require('@test/test-target');

describe('issue 218', function () {
	it('attribute value contains quote should be parsed correct', function () {
		const html = `<html>
  <div id="_" title='"world"' onClick='alert("hello")' color="red">nochange</div>
  <div id="e" title='"world"' color="red">expected</div>
  <div id="a" title='"world"' onClick='alert("hello")' color="red">actual</div>
</html>`;
		const root = parse(html);
		root.toString().should.eql(`<html>
  <div id="_" title='"world"' onClick='alert("hello")' color="red">nochange</div>
  <div id="e" title='"world"' color="red">expected</div>
  <div id="a" title='"world"' onClick='alert("hello")' color="red">actual</div>
</html>`);
		root.querySelector('#e').setAttribute('onClick', "alert('hello')");

		root.toString().should.eql(`<html>
  <div id="_" title='"world"' onClick='alert("hello")' color="red">nochange</div>
  <div id="e" title="&quot;world&quot;" color="red" onClick="alert('hello')">expected</div>
  <div id="a" title='"world"' onClick='alert("hello")' color="red">actual</div>
</html>`);

		// root.querySelector('#a').setAttribute('title', '"replaced"');
		root.querySelector('#a').removeAttribute('color'); // FIXME

		root.toString().should.eql(`<html>
  <div id="_" title='"world"' onClick='alert("hello")' color="red">nochange</div>
  <div id="e" title="&quot;world&quot;" color="red" onClick="alert('hello')">expected</div>
  <div id="a" title="&quot;world&quot;" onClick="alert(&quot;hello&quot;)">actual</div>
</html>`);
		root.querySelector('#a').setAttribute('title', '"replaced"');
		root.toString().should.eql(`<html>
  <div id="_" title='"world"' onClick='alert("hello")' color="red">nochange</div>
  <div id="e" title="&quot;world&quot;" color="red" onClick="alert('hello')">expected</div>
  <div id="a" title="&quot;replaced&quot;" onClick="alert(&quot;hello&quot;)">actual</div>
</html>`);
	});
	it('should escape newlines to html entities', function () {
		const root = parse('<p></p>');
		const p = root.firstChild;
		p.setAttribute('a', '1\n2');
		p.getAttribute('a').should.eql('1\n2');
		p.toString().should.eql('<p a="1\n2"></p>');
	});
});
