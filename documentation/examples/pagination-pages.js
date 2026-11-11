import got from '../../dist/source/index.js';

// `got.paginate.pages()` yields one complete, parsed `Response` per page.
// The next page is downloaded in the background and overlaps the processing
// of the current one; at most one undelivered page is held at a time.

const iterator = got.paginate.pages(
	'https://api.github.com/repos/sindresorhus/got/commits',
	{
		responseType: 'json',
		// Ordinary request options only. The legacy `pagination` object is not
		// accepted here; per-page collection is configured in the third argument.
		retry: {limit: 2},
	},
	{
		prefetch: true, // Default: fetch the successor before consuming the current page.
		requestLimit: 10_000, // Default; retries do not count towards it.
		backoff: 0, // Default; a background-only wait, never delaying page delivery.
		paginate(response) {
			// Synchronous continuation based on the full current `Response`.
			// The default implementation follows the Link `rel="next"` header;
			// here we show a query-string cursor instead.
			const link = response.headers.link;

			if (typeof link !== 'string') {
				return false;
			}

			const match = link.match(/<([^>]+)>;\s*rel="next"/);

			if (!match) {
				return false;
			}

			// A `URL` instance is required when returning a `url` directly.
			return {
				url: new URL(match[1], response.url),
			};
		},
	},
);

for await (const response of iterator) {
	// Full response: parsed body, headers, final url and request.
	console.log(response.url, response.statusCode, response.body.length);

	// Break out (or call `await iterator.return()`) to stop; this cancels the
	// prefetch and any in-flight request without waiting for them.
}
