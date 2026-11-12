const { parse } = require('@test/test-target');

describe('issue 226', function () {
	it('get node line', function () {
		const html = `<div>
	<img src="http://localhost/foo.png" />
	<img src="http://localhost/bar.png" />
	<img src="http://localhost/foo.png" />
	<img src="./foo.png" ></img>

	<img src="./bar.png" ></img>
	<img src="./foo.png" ></img>
</div>`;
		lintHtml(html).should.deepEqual([{
			line: 2,
			message: 'image src "http://localhost/foo.png" uses localhost'
		}, {
			line: 3,
			message: 'image src "http://localhost/bar.png" uses localhost'
		}, {
			line: 4,
			message: 'image src "http://localhost/foo.png" uses localhost'
		}, {
			line: 5,
			message: 'image src "./foo.png" is relative and must be absolute'
		}, {
			line: 7,
			message: 'image src "./bar.png" is relative and must be absolute'
		}, {
			line: 8,
			message: 'image src "./foo.png" is relative and must be absolute'
		}]);
	});
});

/**
 * Get line no of Image elements
 * @param {String} html 
 */
function lintHtml(html) {
	const lint = [];
	// check html for images with relative paths or localhost
	const root = parse(html);
	const images = root.querySelectorAll("img");
	function getLine(node) {
		const r = html.substring(0, node.range[0]).match(/\n/g);
		if (r) {
			return r.length + 1;
		}
		return 1;
	}
	for (const image of images) {
		const src = image.getAttribute("src");
		if (!src) continue;
		if (src.startsWith("http://localhost")) {
			lint.push({
				line: getLine(image),
				message: `image src "${src}" uses localhost`,
			});
		} else if (!src.startsWith("http")) {
			lint.push({
				line: getLine(image),
				message: `image src "${src}" is relative and must be absolute`,
			});
		}
	}
	return lint;
}