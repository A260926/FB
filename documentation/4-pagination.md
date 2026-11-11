[> Back to homepage](../readme.md#documentation)

## Pagination API

Source code: [`source/create.ts`](../source/create.ts)

### How does the `Link` header work?

The [RFC5988](https://datatracker.ietf.org/doc/html/rfc5988#section-5) defines how the `Link` header looks like.

When the response has been processed, Got looks for [the reference of the `next` relation](https://datatracker.ietf.org/doc/html/rfc5988#section-6.2.2).\
This way Got knows the URL it should visit afterwards. The header can look like this:

```
Link: <https://api.github.com/repositories/18193978/commits?page=2>; rel="next", <https://api.github.com/repositories/18193978/commits?page=44>; rel="last"
```

By default, Got looks only at the `next` relation. To use [other relations](https://datatracker.ietf.org/doc/html/rfc5988#section-6.2.2), you need to customize the `paginate` function below.

### `got.paginate(url, options?)`
### `got.paginate.each(url, options?)`

Returns an [async iterator](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/for-await...of).

This is memory efficient, as the logic is executed immediately when new data comes in.

```js
import got from 'got';

const countLimit = 10;

const pagination = got.paginate(
	'https://api.github.com/repos/sindresorhus/got/commits',
	{
		pagination: {countLimit}
	}
);

console.log(`Printing latest ${countLimit} Got commits (newest to oldest):`);

for await (const commitData of pagination) {
	console.log(commitData.commit.message);
}
```

### `got.paginate.all(url, options?)`

**Note:**
> - Querying a large dataset significantly increases memory usage.

Returns a Promise for an array of all results.

```js
import got from 'got';

const countLimit = 10;

const results = await got.paginate.all('https://api.github.com/repos/sindresorhus/got/commits', {
	pagination: {countLimit}
});

console.log(`Printing latest ${countLimit} Got commits (newest to oldest):`);
console.log(results);
```

### `got.paginate.pages(url, options?, pageOptions?)`

Returns an [async iterator](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Statements/for-await...of) of full [`Response`](3-streams.md#response-1) objects — one complete, parsed page per iteration instead of individual items.

Each yielded value is the whole `Response`, including the parsed `body`, `headers`, the final `url` (after redirects) and `request`. This is independent of the `resolveBodyOnly` request option, which is ignored here so you always receive the wrapped response.

Unlike `got.paginate()`, this entry point:

- yields whole pages rather than running item level `transform`, `filter` or `shouldContinue`,
- takes its collection settings from the **third** argument (`pageOptions`); the second argument is ordinary request configuration and does **not** accept the legacy `pagination` object,
- downloads the next page in the background so that downloading overlaps processing of the current page.

`pageOptions` has four fields, all optional:

| Option | Default | Description |
| --- | --- | --- |
| `prefetch` | `true` | Download the next page (and wait out its `backoff`) before the current page is consumed with `next()`. |
| `requestLimit` | `10000` | Maximum number of page requests. Retries do not count towards it. |
| `backoff` | `0` | Milliseconds to wait before requesting the next page. The timer starts as soon as the previous page is fully received and runs in the background, so it never delays delivery of the current page. |
| `paginate` | `Link` header logic (`rel="next"`, resolved against `response.url`) | A **synchronous** function receiving the current full `Response`. Return incremental request options for the next page, or `false` to stop. A returned `url` must be a `URL` instance, just like the legacy `pagination.paginate`. |

The incremental options returned by `paginate` are merged onto a private copy of the previous request options; merging never mutates an already delivered response's `request.options`, the caller's options or the instance defaults. A returned `url` is cloned as well.

#### Prefetch timing and the "one undelivered page" bound

With `prefetch: true` (the default), once a page has been fully received the iterator first computes its successor, then **schedules the next request before handing the current page back to the caller**. While you process the current page, the next page downloads in the background.

At any instant the iterator holds **at most one** undelivered response or in-flight request. Even if that successor finishes downloading very quickly, its `paginate` callback is not run and a third page is never requested until you actually take the successor with `next()`. For example, if page 1 links to page 2 and page 2 links to page 3 but you stop while holding page 1, the most that can ever happen is the download of page 2 — page 3 is never touched.

An empty array (`[]`) or empty object (`{}`) body is still a normal page; the iterator never ends on its own based on body length. The collection ends normally only when `paginate` returns `false` or when `requestLimit` is reached (reaching the limit never invokes `paginate` again).

Set `prefetch: false` to request each page strictly on demand: the next request (and any backoff) does not begin until the following `next()` call.

#### Stopping a collection

Break out of a `for await…of` loop (or call `iterator.return()`) to terminate the collection. This immediately enters a terminated state, cancels the in-flight Got request and any background backoff timer owned by the iterator, and settles a still-pending `next()` as `done`. A `return()` does not wait for a stalled network read or a long backoff to finish naturally. The caller's `AbortController`, the shared agent and other requests on the same Got instance are not affected. Repeating `return()` stays successful, and every later `next()` resolves `done: true`.

If you instead abort an external `AbortSignal` passed through `options.signal`, the waiting `next()` rejects with Got's `AbortError` (`code: 'ERR_ABORTED'`); when there is no pending `next()`, the error is thrown once by the following `next()`, after which the iterator ends.

A failure of a prefetched page — an HTTP error, a body parse error or a `paginate` callback that throws — is captured immediately. It never surfaces as an unhandled rejection and never disturbs the page already in hand. When you ask for that successor with `next()`, the same error is thrown once and the iterator ends. If you `return()` first, the undelivered error is discarded.

#### TypeScript example

```ts
import got, {type Response} from 'got';

type Item = {id: number; name: string};
type PageBody = {items: Item[]};

const pages = got.paginate.pages<PageBody>(
	'https://api.example.com/items',
	{
		responseType: 'json',
		// Ordinary request options only; no legacy `pagination` object here.
		retry: {limit: 2},
	},
	{
		// Defaults shown for clarity; all are optional.
		prefetch: true,
		requestLimit: 10_000,
		backoff: 0,
		paginate(response: Response<PageBody>) {
			const next = response.headers['x-next-page'];

			if (!next) {
				return false; // No more pages.
			}

			// Incremental options: just the URL change. A `URL` instance is
			// required when you set `url` directly.
			return {
				url: new URL(next, response.url),
			};
		},
	},
);

for await (const response of pages) {
	// `response` is the full page: parsed body, headers, final url and request.
	console.log(response.url, response.statusCode);

	for (const item of response.body.items) {
		// While this runs, the next page is already downloading in the background.
		await processItem(item);
	}
}

async function processItem(item: Item): Promise<void> {
	// Your downstream work, which may be slower than the download.
}
```

The same iterator can be driven manually, which is useful when you want explicit control over stopping:

```ts
const iterator = got.paginate.pages<PageBody>('https://api.example.com/items', {
	responseType: 'json'
});

let result = await iterator.next();
while (!result.done) {
	const response = result.value; // Response<PageBody>

	if (shouldStop(response)) {
		await iterator.return(); // Cancels the prefetch and any in-flight request.
		break;
	}

	result = await iterator.next();
}
```

### `pagination`

**Type: `object`**\
**Default:**

```js
{
	transform: (response: Response) => {
		if (response.request.options.responseType === 'json') {
			return response.body;
		}

		return JSON.parse(response.body as string);
	},
	paginate: ({response}) => {
		const rawLinkHeader = response.headers.link;
		if (typeof rawLinkHeader !== 'string' || rawLinkHeader.trim() === '') {
			return false;
		}

		const parsed = parseLinkHeader(rawLinkHeader);
		const next = parsed.find(entry => entry.parameters.rel === 'next' || entry.parameters.rel === '"next"');

		if (next) {
			return {
				url: new URL(next.reference, response.requestUrl)
			};
		}

		return false;
	},
	filter: () => true,
	shouldContinue: () => true,
	countLimit: Number.POSITIVE_INFINITY,
	backoff: 0,
	requestLimit: 10_000,
	stackAllItems: false
}
```

This option represents the `pagination` object.

#### `transform`

**Type: `Function`**\
**Default: `response => JSON.parse(response.body)`**

A function that transforms [`Response`](3-streams.md#response-1) into an array of items.\
This is where you should do the parsing.

#### `paginate`

**Type: `Function`**\
**Default: `Link` header logic**

The function takes an object with the following properties:

- `response` - The current response object,
- `currentItems` - Items from the current response,
- `allItems` - An empty array, unless `stackAllItems` is `true`, otherwise it contains all emitted items.

It should return an object representing Got options pointing to the next page. If there is no next page, `false` should be returned instead.

The options are merged automatically with the previous request.\
Therefore the options returned by `pagination.paginate(…)` must reflect changes only.

**Note:**
> - The `url` option (if set) accepts **only** a [`URL`](https://developer.mozilla.org/en-US/docs/Web/API/URL) instance.\
>   This prevents `prefixUrl` ambiguity. In order to use a relative URL string, merge it via `new URL(relativeUrl, response.url)`.

#### `filter`

**Type: `Function`**\
**Default: `({item, currentItems, allItems}) => true`**

Whether the item should be emitted or not.

#### `shouldContinue`

**Type: `Function`**\
**Default: `({item, currentItems, allItems}) => true`**

**Note:**
> - This function executes only when `filter` returns `true`.

For example, if you need to stop before emitting an entry with some flag, you should use `({item}) => !item.flag`.

If you want to stop after emitting the entry, you should use `({item, allItems}) => allItems.some(item => item.flag)` instead.

#### `countLimit`

**Type: `number`**\
**Default: `Number.POSITIVE_INFINITY`**

The maximum amount of items that should be emitted.

#### `backoff`

**Type: `number`**\
**Default: `0`**

Milliseconds to wait before the next request is triggered.

#### `requestLimit`

**Type: `number`**\
**Default: `10000`**

The maximum amount of request that should be triggered.

**Note:**
> - [Retries on failure](7-retry.md) are not counted towards this limit.

#### `stackAllItems`

**Type: `boolean`**\
**Default: `false`**

Defines how `allItems` is managed in `pagination.paginate`, `pagination.filter` and `pagination.shouldContinue`.

By default, `allItems` is always an empty array. Setting this to `true` will significantly increase memory usage when working with a large dataset.

### Example

In this example we will use `searchParams` instead of `Link` header.\
Just to show how you can customize the `paginate` function.

The reason `filter` looks exactly the same like `shouldContinue` is that the latter will tell Got to stop once we reach our timestamp.
The `filter` function is needed as well, because in the same response we can get results with different timestamps.

```js
import got from 'got';
import Bourne from '@hapi/bourne';

const max = Date.now() - 1000 * 86400 * 7;

const iterator = got.paginate('https://api.github.com/repos/sindresorhus/got/commits', {
	pagination: {
		paginate: ({response, currentItems}) => {
			// If there are no more data, finish.
			if (currentItems.length === 0) {
				return false;
			}

			// Get the current page number.
			const {searchParams} = response.request.options;
			const previousPage = Number(searchParams.get('page') ?? 1);

			// Update the page number by one.
			return {
				searchParams: {
					page: previousPage + 1
				}
			};
		},
		// Using `Bourne` to prevent prototype pollution.
		transform: response => Bourne.parse(response.body),
		filter: ({item}) => {
			// Check if the commit time exceeds our range.
			const date = new Date(item.commit.committer.date);
			const end = date.getTime() - max >= 0;

			return end;
		},
		shouldContinue: ({item}) => {
			// Check if the commit time exceeds our range.
			const date = new Date(item.commit.committer.date);
			const end = date.getTime() - max >= 0;

			return end;
		},
		// We want only 50 results.
		countLimit: 50,
		// Wait 1s before making another request to prevent API rate limiting.
		backoff: 1000,
		// It is a good practice to set an upper limit of how many requests can be made.
		// This way we can avoid infinite loops.
		requestLimit: 10,
		// In this case, we don't need to store all the items we receive.
		// They are processed immediately.
		stackAllItems: false
	}
});

console.log('Last 50 commits from now to week ago:');
for await (const item of iterator) {
	console.log(item.commit.message.split('\n')[0]);
}
```
