const { parse } = require('@test/test-target');

// https://github.com/taoqf/node-html-parser/issues/203
describe('issue 203', function () {
	it('code element should not be null', function () {
		const root = parse(`<html><body><pre><code class="language-typescript">type Foo = { foo: 'bar' }</code></pre></body></html>`, {
			blockTextElements: {
				script: true,
				noscript: true,
				style: true,
			}
		});
		const t = root.firstChild.firstChild.firstChild.firstChild;
		t.toString().should.eql(`<code class="language-typescript">type Foo = { foo: 'bar' }</code>`);

		const code = root.querySelector("code");
		code.toString().should.eql(`<code class="language-typescript">type Foo = { foo: 'bar' }</code>`);
	});
	it('code element should not be null', function () {
		const root = parse(` <div class="clip_details-description description-wrapper iris_desc">
                    <p class="first">Country music legend, Trish Cotton, has something to say.</p>
                    <p>
                      Written by Kyle Kasabian (@kylekasabian) <br />
                      Directed by Derek Mari (@directorderek)<br />
                      Director of Photography: Peter Mickelsen<br />
                      Produced by Derek Mari and Kyle Kasabian<br />
                      Edited by Derek Mari
                    </p>
                    <p>Starring: Alyssa Sabo, Janine Hogan, and Kyle Kasabian</p>
                    <p>
                        Assistant Camera: Casey Schoch<br />
                        Production Sound: David Alvarez<br />
                        Production Assistant: Keith Ahlstrom
                    </p>
                    <p>Music by Morgan Matthews</p>
                    <p>
                      Blink &amp; Miss Productions<br />
                      Bad Cat Films
                    </p>
                  </div>
                </div>`,);
		const description = root.querySelector('.description-wrapper');
		description.toString().should.not.eql(null);
	});
});
