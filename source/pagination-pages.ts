import {AbortError} from './core/errors.js';
import Options, {type OptionsInit} from './core/options.js';
import parseLinkHeader from './core/parse-link-header.js';
import type {Response} from './core/response.js';
import type {Got} from './types.js';
import type {CancelableRequest} from './as-promise/types.js';
import type Request from './core/index.js';

/**
The request configuration accepted by `got.paginate.pages()`.

It is ordinary request options: the legacy item-level `pagination` config is not
accepted here and has no effect. Page collection is configured exclusively via
the third argument.
*/
export type PageRequestOptions = Omit<OptionsInit, 'pagination'>;

/**
The synchronous continuation function used by `got.paginate.pages()`.

It receives the full, parsed `Response` of the current page and returns either
the incremental options for the next request or `false` when there are no more
pages.
*/
export type PagePaginateFunction<R> = (response: Response<R>) => OptionsInit | false;

/**
Per-page collection options for `got.paginate.pages()`.

These are intentionally separate from the legacy `pagination` option: the item
level `transform`, `filter` and `shouldContinue` hooks are never executed here.
*/
export type PagePaginationOptions<R = unknown> = {
	/**
	Whether the next page is downloaded (and its backoff waited out) in the
	background before the current page is consumed with `next()`.

	@default true
	*/
	prefetch?: boolean;

	/**
	The maximum amount of requests that may be triggered. Retries do not count
	towards this limit.

	@default 10000
	*/
	requestLimit?: number;

	/**
	Milliseconds to wait before requesting the next page. The timer starts as
	soon as the current page has been fully received and runs in the background,
	so it never delays delivery of the current page.

	@default 0
	*/
	backoff?: number;

	/**
	A synchronous function receiving the current full `Response` and returning
	either incremental request options for the next page or `false` to stop.

	@default Link header (`rel="next"`) resolution, relative to `response.url`
	*/
	paginate?: PagePaginateFunction<R>;
};

const defaultPaginate: PagePaginateFunction<unknown> = (response: Response): OptionsInit | false => {
	const rawLinkHeader = response.headers.link;

	if (typeof rawLinkHeader !== 'string' || rawLinkHeader.trim() === '') {
		return false;
	}

	const parsed = parseLinkHeader(rawLinkHeader);
	const next = parsed.find(entry => entry.parameters.rel === 'next' || entry.parameters.rel === '"next"');

	if (next) {
		return {
			url: new URL(next.reference, response.url),
		};
	}

	return false;
};

// A background task only ever downloads a single page. The continuation
// (`paginate`) for that page is computed by `advance()` once the page is
// actually consumed, so an early download never advances the chain.
type PreparedPage<R> =
	| {kind: 'page'; response: Response<R>}
	| {kind: 'end'}
	| {kind: 'error'; error: unknown};

type Task<R> = {
	controller: AbortController;
	promise: Promise<PreparedPage<R>>;
};

const waitWithSignal = (milliseconds: number, signal: AbortSignal): Promise<void> => new Promise(resolve => {
	if (milliseconds === 0 || signal.aborted) {
		resolve();
		return;
	}

	const timer = setTimeout(() => {
		signal.removeEventListener('abort', onAbort);
		resolve();
	}, milliseconds);

	const onAbort = (): void => {
		clearTimeout(timer);
		signal.removeEventListener('abort', onAbort);
		resolve();
	};

	signal.addEventListener('abort', onAbort, {once: true});
});

const createSyntheticAbortError = (options: Options): AbortError => {
	// Reuse Got's own error shape so callers handle it exactly like an abort
	// raised by an in-flight request, even when the abort lands while the
	// backoff timer (with no request yet) is running. The `_onResponse` marker
	// is how Got's `RequestError` distinguishes a request-like object.
	const requestLike = {_onResponse() {}, options} as unknown as Request;
	return new AbortError(requestLike);
};

/**
The async iterator returned by `got.paginate.pages()`. It mirrors the standard
async iterator contract but guarantees an explicit, always-present `return()`
so a collection can be terminated deterministically.
*/
export type PagesIterator<R> = AsyncIterableIterator<Response<R>> & {
	next(): Promise<IteratorResult<Response<R>>>;
	return(value?: unknown): Promise<IteratorResult<Response<R>>>;
};

/**
Arguments for building a `got.paginate.pages()` iterator, grouped so the
factory stays a single-parameter function.
*/
export type CreatePagesOptions<R> = {
	got: Got;
	defaultOptions: Options;
	url: string | URL;
	options?: PageRequestOptions;
	pageOptions?: PagePaginationOptions<R>;
};

export default function createPagesIterator<R>({
	got,
	defaultOptions,
	url,
	options,
	pageOptions = {},
}: CreatePagesOptions<R>): PagesIterator<R> {
	const {
		prefetch = true,
		requestLimit = 10_000,
		backoff = 0,
		paginate = defaultPaginate as PagePaginateFunction<R>,
	} = pageOptions;

	// Validate before option normalization, so an invalid configuration never
	// triggers a single request.
	if (typeof requestLimit !== 'number' || !Number.isSafeInteger(requestLimit) || requestLimit <= 0) {
		throw new TypeError('`pageOptions.requestLimit` must be a positive safe integer');
	}

	if (typeof backoff !== 'number' || !Number.isFinite(backoff) || backoff < 0) {
		throw new TypeError('`pageOptions.backoff` must be a non-negative finite number');
	}

	if (typeof prefetch !== 'boolean') {
		throw new TypeError('`pageOptions.prefetch` must be a boolean');
	}

	if (typeof paginate !== 'function') {
		throw new TypeError('`pageOptions.paginate` must be a function');
	}

	// Every page is delivered as a full `Response`, regardless of `resolveBodyOnly`.
	const initialOptions = new Options(url, options, defaultOptions);
	initialOptions.resolveBodyOnly = false;

	// Options used as the base for the next request. It is a private, per-iterator
	// chain: never the caller's options object, the instance defaults or any
	// delivered response's `request.options`.
	let baseOptions = initialOptions;
	let requestCount = 0;

	// Continuation computed for the most recently delivered page and the URL it
	// was served from. Used only when prefetching is disabled.
	let pendingContinuation: OptionsInit | false | undefined;
	let previousUrl: string | URL | undefined;

	let ended = false;
	let closed = false;
	let externalAborted = false;
	let abortError: AbortError | undefined;
	let inflightNext: Promise<IteratorResult<Response<R>>> | undefined;

	let activeTask: Task<R> | undefined;
	let activeRequest: CancelableRequest<Response<R>> | undefined;

	const externalSignal = initialOptions.signal;

	const onExternalAbort = (): void => {
		externalAborted = true;
	};

	const getAbortError = (): AbortError => {
		if (abortError === undefined) {
			abortError = createSyntheticAbortError(baseOptions);
		}

		return abortError;
	};

	externalSignal?.addEventListener('abort', onExternalAbort, {once: true});

	const startRequest = async (signal: AbortSignal): Promise<Response<R>> => {
		// A fresh `Options` instance per request. It is never the delivered
		// response's `request.options`, and merging later continuations can never
		// mutate a previously delivered page.
		const requestOptions = new Options(undefined, undefined, baseOptions);

		// `cloneInternals` shares the underlying `URL` instance. Detach it so this
		// request owns an independent URL/query object from birth.
		if (requestOptions.url !== undefined) {
			requestOptions.prefixUrl = '';
			requestOptions.url = new URL(requestOptions.url.toString());
		}

		requestOptions.resolveBodyOnly = false;
		requestOptions.signal = signal;

		const request = got(undefined, undefined, requestOptions) as CancelableRequest<Response<R>>;
		activeRequest = request;
		requestCount++;

		try {
			return await request;
		} catch (error) {
			if (externalAborted && abortError === undefined) {
				abortError = error as AbortError;
			}

			throw error;
		} finally {
			if (activeRequest === request) {
				activeRequest = undefined;
			}
		}
	};

	const buildNextOptions = (continuation: OptionsInit): Options => {
		const nextOptions = new Options(undefined, undefined, baseOptions);

		// Detach the shared `URL` before merging, so the continuation cannot
		// rewrite the previous page's URL or query string.
		nextOptions.prefixUrl = '';
		nextOptions.url = new URL(previousUrl!.toString());

		nextOptions.merge(continuation);

		if (continuation.url !== undefined) {
			if (!(continuation.url instanceof URL)) {
				throw new TypeError('The `url` returned by `paginate` must be a `URL` instance');
			}

			// Clone it so the caller's/handler's URL object can never alias the
			// request options used (and later merged) by this iterator.
			nextOptions.prefixUrl = '';
			nextOptions.url = new URL(continuation.url.toString());
		}

		nextOptions.resolveBodyOnly = false;

		return nextOptions;
	};

	// Waits out the backoff, builds the next options (when continuing) and
	// downloads exactly one page. Every failure is captured, so a background
	// task can never produce an unhandled rejection.
	const runTask = async (controller: AbortController, continuation: OptionsInit | undefined): Promise<PreparedPage<R>> => {
		const taskSignal = externalSignal ? AbortSignal.any([externalSignal, controller.signal]) : controller.signal;

		if (requestCount > 0) {
			// The timer starts as soon as the previous page finished and is
			// interruptible by both `return()` and external abort, so it can never
			// block delivery or a pending `next()`.
			await waitWithSignal(backoff, taskSignal);

			if (closed) {
				return {kind: 'end'};
			}
		}

		if (externalAborted) {
			return {kind: 'error', error: getAbortError()};
		}

		if (requestCount >= requestLimit) {
			return {kind: 'end'};
		}

		if (continuation !== undefined) {
			try {
				baseOptions = buildNextOptions(continuation);
			} catch (error) {
				return {kind: 'error', error};
			}
		}

		try {
			const response = await startRequest(taskSignal);

			if (closed) {
				return {kind: 'end'};
			}

			return {kind: 'page', response};
		} catch (error) {
			return {kind: 'error', error};
		}
	};

	const startTask = (continuation: OptionsInit | undefined): Task<R> => {
		const controller = new AbortController();
		const promise = runTask(controller, continuation);
		// Keep a handler attached so a settlement after `return()` can never leak
		// as an unhandled rejection. The real result is consumed by `advance()`.
		promise.catch(() => {});

		return {controller, promise};
	};

	// Resolves the task that will produce the next page, or `undefined` when the
	// collection has naturally ended before another request is needed. The task
	// stays assigned to `activeTask` for its whole life (including while awaited)
	// so a concurrent `return()` can always abort it.
	const acquireTask = (): Task<R> | undefined => {
		if (prefetch) {
			// The first `next()` starts the first request. Every later `next()`
			// consumes the successor scheduled right after the previous page.
			if (activeTask === undefined) {
				if (requestCount === 0) {
					activeTask = startTask(undefined);
				} else {
					return undefined;
				}
			}

			return activeTask;
		}

		if (requestCount > 0 && pendingContinuation === false) {
			return undefined;
		}

		activeTask = startTask(requestCount === 0 ? undefined : pendingContinuation as OptionsInit);
		return activeTask;
	};

	// Computes the successor of a freshly downloaded page and (when prefetching)
	// schedules its download before the current page is handed to the caller.
	const deliverPage = (response: Response<R>): IteratorResult<Response<R>> => {
		// At the limit we stop without consulting `paginate`.
		const continuation: OptionsInit | false = requestCount >= requestLimit
			? false
			: paginate(response);

		previousUrl = response.request.options.url;
		pendingContinuation = continuation;

		if (prefetch && continuation !== false && requestCount < requestLimit) {
			// Schedule the successor before handing over the current page, so the
			// download overlaps the caller's processing. At most one undelivered
			// page / in-flight request is ever held.
			activeTask = startTask(continuation);
		}

		return {value: response, done: false};
	};

	const advance = async (): Promise<IteratorResult<Response<R>>> => {
		if (closed) {
			return {value: undefined, done: true};
		}

		if (externalAborted) {
			ended = true;
			throw getAbortError();
		}

		try {
			const task = acquireTask();

			if (task === undefined) {
				ended = true;
				return {value: undefined, done: true};
			}

			const prepared = await task.promise;

			// The task has settled and no longer needs aborting. Drop it so a
			// consumed task can never be re-yielded; `deliverPage` may immediately
			// schedule the next prefetch task in its place.
			if (activeTask === task) {
				activeTask = undefined;
			}

			if (closed) {
				return {value: undefined, done: true};
			}

			if (externalAborted) {
				ended = true;
				throw getAbortError();
			}

			if (prepared.kind === 'error') {
				ended = true;
				throw prepared.error;
			}

			if (prepared.kind === 'end') {
				ended = true;
				return {value: undefined, done: true};
			}

			return deliverPage(prepared.response);
		} catch (error) {
			if (closed) {
				return {value: undefined, done: true};
			}

			ended = true;
			throw error;
		}
	};

	const cleanup = (): void => {
		closed = true;
		externalSignal?.removeEventListener('abort', onExternalAbort);

		// Cancel the in-flight Got request and the background backoff wait owned
		// by this iterator. The resulting rejections are observed below and by the
		// task handlers, so nothing leaks. Shared agents and other requests are
		// untouched.
		activeTask?.controller.abort();
		activeRequest?.cancel();
		activeTask?.promise.catch(() => {});

		// The pending `next()` (if any) observes `closed` and settles `done: true`.
		// Observe it here as well in case the caller only awaited `return()`.
		inflightNext?.catch(() => {});

		activeTask = undefined;
		activeRequest = undefined;
	};

	const iterator: PagesIterator<R> = {
		[Symbol.asyncIterator]() {
			return this;
		},

		next(): Promise<IteratorResult<Response<R>>> {
			if (closed || ended) {
				return Promise.resolve({value: undefined, done: true});
			}

			// Concurrent `next()` calls are not supported, but reuse the pending
			// operation so `return()` can still race a single in-flight `next()`.
			if (inflightNext !== undefined) {
				return inflightNext;
			}

			const promise = advance().finally(() => {
				if (inflightNext === promise) {
					inflightNext = undefined;
				}

				// Release the external-signal listener once the collection reaches
				// a natural end or an error (`return()` removes it in `cleanup()`).
				if (ended) {
					externalSignal?.removeEventListener('abort', onExternalAbort);
				}
			});

			inflightNext = promise;

			return promise;
		},

		async return(value?: unknown): Promise<IteratorResult<Response<R>>> {
			if (closed) {
				return {value: undefined, done: true};
			}

			cleanup();

			return {value: value as Response<R> | undefined, done: true};
		},
	};

	return iterator;
}
