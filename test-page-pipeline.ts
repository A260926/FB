import process from 'node:process';
import assert from 'node:assert/strict';
import http from 'node:http';
import {Duplex} from 'node:stream';
import test from 'node:test';
import got, {type OptionsInit, type Response} from './source/index.js';

// The sandbox used to develop this does not allow binding TCP ports or Unix
// sockets, so the tests drive a real `http.Server` entirely in-process: a pair
// of connected duplex streams stands in for the socket, and a custom agent
// hands Got the client end while the server end is fed through
// `server.emit('connection')`. All Got semantics (handlers, hooks, retries,
// parsing, abort and cancellation) run unchanged on top of real HTTP messages.

const BASE_URL = 'http://page-test.local';

type SocketSide = Duplex & {
	encrypted?: boolean;
	server?: http.Server;
	setNoDelay: () => void;
	setKeepAlive: () => void;
	ref: () => SocketSide;
	unref: () => SocketSide;
	remoteAddress: string;
	localAddress: string;
	remotePort: number;
	localPort: number;
};

const createSocketPair = (): [SocketSide, SocketSide] => {
	let clientSide: SocketSide;
	let serverSide: SocketSide;

	clientSide = new Duplex({
		read() {},
		write(chunk, _encoding, callback) {
			serverSide.push(chunk);
			callback();
		},
		final(callback) {
			serverSide.push(null);
			callback();
		},
	}) as SocketSide;

	serverSide = new Duplex({
		read() {},
		write(chunk, _encoding, callback) {
			clientSide.push(chunk);
			callback();
		},
		final(callback) {
			clientSide.push(null);
			callback();
		},
	}) as SocketSide;

	for (const side of [clientSide, serverSide]) {
		side.setNoDelay = () => {};
		side.setKeepAlive = () => {};
		side.ref = () => side;
		side.unref = () => side;
		side.remoteAddress = '127.0.0.1';
		side.localAddress = '127.0.0.1';
		side.remotePort = 12_345;
		side.localPort = 80;
		side.on('error', () => {});
	}

	// Mimic a real socket: destroying one end tears down the other, so a client
	// cancellation surfaces as a `close` on the server-side response.
	let destroying = false;

	const destroyClient = clientSide.destroy.bind(clientSide);
	clientSide.destroy = (error?: Error | null) => {
		destroyClient(error ?? undefined);

		if (!destroying) {
			destroying = true;
			serverSide.destroy(error ?? undefined);
			destroying = false;
		}

		return clientSide;
	};

	const destroyServer = serverSide.destroy.bind(serverSide);
	serverSide.destroy = (error?: Error | null) => {
		destroyServer(error ?? undefined);

		if (!destroying) {
			destroying = true;
			clientSide.destroy(error ?? undefined);
			destroying = false;
		}

		return serverSide;
	};

	return [clientSide, serverSide];
};

type RecordedRequest = {
	url: string;
	headers: http.IncomingHttpHeaders;
	closed: boolean;
};

type InProcessServer = {
	base: string;
	agent: http.Agent;
	requests: RecordedRequest[];
	close: () => Promise<void>;
};

const startInProcessServer = (
	handler: (request: http.IncomingMessage, response: http.ServerResponse) => void,
): InProcessServer => {
	const requests: RecordedRequest[] = [];

	const server = http.createServer((request, response) => {
		const recorded: RecordedRequest = {
			url: request.url ?? '',
			headers: request.headers,
			closed: false,
		};
		requests.push(recorded);
		response.once('close', () => {
			recorded.closed = true;
		});
		handler(request, response);
	});

	const agent = new http.Agent({keepAlive: false}) as http.Agent & {
		createConnection: (
			options: http.ClientRequestArgs,
			callback?: (error: Error | null, socket: Duplex) => void,
		) => Duplex;
	};
	agent.createConnection = (_options, callback) => {
		const [clientSide, serverSide] = createSocketPair();
		serverSide.encrypted = false;
		serverSide.server = server;
		server.emit('connection', serverSide);

		if (callback) {
			process.nextTick(callback, null, clientSide);
		}

		return clientSide;
	};

	return {
		base: BASE_URL,
		agent,
		requests,
		async close() {
			// The server is driven in-process and never calls `listen()`, so only
			// close it when something actually bound it.
			if (server.listening) {
				server.closeAllConnections();
				await new Promise<void>((resolve, reject) => {
					server.close(error => (error ? reject(error) : resolve()));
				});
			}

			agent.destroy();
		},
	};
};

const barrier = (): {promise: Promise<void>; release: () => void} => {
	let release!: () => void;
	const promise = new Promise<void>(resolve => {
		release = resolve;
	});
	return {promise, release};
};

const flush = async (turns = 8): Promise<void> => {
	for (let index = 0; index < turns; index++) {
		// eslint-disable-next-line no-await-in-loop
		await new Promise(resolve => setImmediate(resolve));
	}
};

const json = (response: http.ServerResponse, value: unknown): void => {
	response.setHeader('content-type', 'application/json');
	response.end(JSON.stringify(value));
};

const urls = (server: InProcessServer): string[] => server.requests.map(request => request.url);

const captureRejects = async (promise: Promise<unknown>): Promise<Error & {code?: string; options?: unknown}> => {
	try {
		await promise;
	} catch (error) {
		return error as Error & {code?: string; options?: unknown};
	}

	throw new Error('Expected the promise to reject');
};

// Collects any unhandled rejection produced while the tests run.
const unhandled: unknown[] = [];
const onUnhandledRejection = (reason: unknown): void => {
	unhandled.push(reason);
};
process.on('unhandledRejection', onUnhandledRejection);

test.after(async () => {
	process.removeListener('unhandledRejection', onUnhandledRejection);
	// Give late settlements a chance to surface (they must not).
	await flush();
	assert.deepEqual(unhandled, [], `unhandled rejections leaked: ${unhandled.map(String).join(' | ')}`);
});

test('page pipeline overlap', async t => {
	// Page 2 stays open until a barrier is released, proving that even with an
	// in-flight successor, page 3 is never touched until page 2 is consumed.
	const pageTwoBarrier = barrier();

	const server = startInProcessServer((request, response) => {
		if (request.url === '/p1') {
			response.setHeader('link', '</p2>; rel="next"');
			json(response, {page: 1});
		} else if (request.url === '/p2') {
			response.setHeader('link', '</p3>; rel="next"');
			response.writeHead(200, {'content-type': 'application/json'});
			response.write('{"page":2');
			pageTwoBarrier.promise.then(() => response.end('}'));
		} else if (request.url === '/p3') {
			json(response, {page: 3});
		} else {
			response.writeHead(404).end();
		}
	});

	const client = got.extend({
		agent: {http: server.agent},
		responseType: 'json',
		retry: {limit: 0},
	});

	try {
		await t.test('creates the iterator without sending a request', async () => {
			const iterator = client.paginate.pages(`${server.base}/p1`);
			await flush();
			assert.deepEqual(urls(server), []);
			await iterator.return();
		});

		await t.test('prefetches exactly one successor and never jumps two pages ahead', async () => {
			const iterator = client.paginate.pages<{page: number}>(`${server.base}/p1`);

			const first = await iterator.next();
			assert.equal(first.done, false);
			assert.deepEqual(first.value!.body, {page: 1});

			await flush();
			// Page 2 has started (its body is partially downloaded)...
			assert.deepEqual(urls(server), ['/p1', '/p2']);
			// ...but page 3 must remain blocked while page 2 is undelivered.
			assert.equal(urls(server).includes('/p3'), false);

			pageTwoBarrier.release();
			const second = await iterator.next();
			assert.deepEqual(second.value!.body, {page: 2});

			await flush();
			// Only now, after taking page 2, may page 3 be requested.
			assert.deepEqual(urls(server), ['/p1', '/p2', '/p3']);

			const third = await iterator.next();
			assert.deepEqual(third.value!.body, {page: 3});
			assert.equal((await iterator.next()).done, true);
		});

		await t.test('does not prefetch when prefetch is disabled', async () => {
			const localServer = startInProcessServer((request, response) => {
				if (request.url === '/q1') {
					response.setHeader('link', '</q2>; rel="next"');
					json(response, {page: 1});
				} else if (request.url === '/q2') {
					response.setHeader('link', '</q3>; rel="next"');
					json(response, {page: 2});
				} else {
					json(response, {page: 3});
				}
			});

			const localClient = got.extend({
				agent: {http: localServer.agent},
				responseType: 'json',
				retry: {limit: 0},
			});

			try {
				const iterator = localClient.paginate.pages<{page: number}>(`${localServer.base}/q1`, undefined, {
					prefetch: false,
				});

				const first = await iterator.next();
				assert.deepEqual(first.value!.body, {page: 1});
				await flush();
				// The successor request waits for the next `next()`.
				assert.deepEqual(urls(localServer), ['/q1']);

				const second = await iterator.next();
				assert.deepEqual(second.value!.body, {page: 2});
				assert.deepEqual(urls(localServer), ['/q1', '/q2']);

				await iterator.next();
				assert.deepEqual(urls(localServer), ['/q1', '/q2', '/q3']);
				assert.equal((await iterator.next()).done, true);
			} finally {
				await localServer.close();
			}
		});

		await t.test('runs the backoff in the background without delaying the current page', async () => {
			const backoffServer = startInProcessServer((request, response) => {
				if (request.url === '/b1') {
					response.setHeader('link', '</b2>; rel="next"');
					json(response, {page: 1});
				} else {
					json(response, {page: 2});
				}
			});

			const backoffClient = got.extend({
				agent: {http: backoffServer.agent},
				responseType: 'json',
				retry: {limit: 0},
			});

			const backoff = 120;
			const iterator = backoffClient.paginate.pages<{page: number}>(`${backoffServer.base}/b1`, undefined, {backoff});

			try {
				const start = Date.now();
				const first = await iterator.next();
				const firstDeliveredAt = Date.now();

				// The first page is delivered immediately; its own download is
				// not held back by the backoff.
				assert.ok(firstDeliveredAt - start < backoff, 'first page delivery was delayed by the backoff');
				assert.deepEqual(first.value!.body, {page: 1});

				await flush();
				assert.deepEqual(urls(backoffServer), ['/b1']);

				const second = await iterator.next();
				const secondDeliveredAt = Date.now();
				assert.deepEqual(second.value!.body, {page: 2});
				// The successor request respected the backoff measured from the
				// moment the first page was received.
				assert.ok(secondDeliveredAt - firstDeliveredAt >= backoff - 30, 'backoff was not waited out');
				assert.equal((await iterator.next()).done, true);
			} finally {
				await iterator.return();
				await backoffServer.close();
			}
		});
	} finally {
		pageTwoBarrier.release();
		await server.close();
	}
});

test('page pipeline continuation', async t => {
	await t.test('resolves relative Link headers against response.url', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/rel?page=1') {
				response.setHeader('link', '<?page=2>; rel="next"');
				json(response, [1]);
			} else if (request.url === '/rel?page=2') {
				json(response, [2]);
			} else {
				response.writeHead(404).end();
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages<number[]>(`${server.base}/rel?page=1`);
			const first = await iterator.next();
			assert.equal(first.value!.url, `${server.base}/rel?page=1`);
			const second = await iterator.next();
			assert.equal(second.value!.url, `${server.base}/rel?page=2`);
			assert.deepEqual(second.value!.body, [2]);
			assert.equal((await iterator.next()).done, true);
			assert.deepEqual(urls(server), ['/rel?page=1', '/rel?page=2']);
		} finally {
			await server.close();
		}
	});

	await t.test('treats empty arrays and empty objects as normal pages', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/e1') {
				response.setHeader('link', '</e2>; rel="next"');
				json(response, []);
			} else if (request.url === '/e2') {
				response.setHeader('link', '</e3>; rel="next"');
				json(response, {});
			} else {
				json(response, {done: true});
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/e1`);
			const pages: unknown[] = [];
			let result = await iterator.next();
			while (!result.done) {
				pages.push(result.value!.body);
				result = await iterator.next();
			}

			assert.deepEqual(pages, [[], {}, {done: true}]);
			assert.deepEqual(urls(server), ['/e1', '/e2', '/e3']);
		} finally {
			await server.close();
		}
	});

	await t.test('applies custom searchParams, headers and url increments', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/inc') {
				json(response, {page: 1});
			} else if (request.url === '/inc?cursor=q2') {
				assert.equal(request.headers['x-page'], '2');
				json(response, {page: 2});
			} else if (request.url === '/explicit?z=1') {
				json(response, {page: 3});
			} else {
				response.writeHead(404).end(request.url);
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});
		const returnedIncrement: OptionsInit = {
			searchParams: {cursor: 'q2'},
			headers: {'x-page': '2'},
		};
		const incrementSnapshot = JSON.stringify(returnedIncrement);

		try {
			const iterator = client.paginate.pages(`${server.base}/inc`, undefined, {
				prefetch: false,
				paginate(response) {
					if (response.url.endsWith('/inc')) {
						return returnedIncrement;
					}

					if (response.url.includes('cursor=q2')) {
						return {url: new URL('/explicit?z=1', response.url)};
					}

					return false;
				},
			});

			await iterator.next();
			await iterator.next();
			assert.equal(server.requests[1]?.headers['x-page'], '2');
			await iterator.next();
			assert.deepEqual(urls(server), ['/inc', '/inc?cursor=q2', '/explicit?z=1']);
			assert.equal((await iterator.next()).done, true);

			// The object returned by the continuation is not mutated by Got.
			assert.equal(JSON.stringify(returnedIncrement), incrementSnapshot);
		} finally {
			await server.close();
		}
	});

	await t.test('keeps delivered response options, caller options and defaults isolated', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/iso?page=1') {
				response.setHeader('link', '</iso?page=2>; rel="next"');
				json(response, {page: 1});
			} else {
				json(response, {page: 2});
			}
		});

		const callerOptions: OptionsInit = {
			headers: {'x-caller': 'keep'},
			searchParams: {page: 1},
		};
		const callerSnapshot = JSON.stringify(callerOptions);

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/iso`, callerOptions, {prefetch: false});
			const first = await iterator.next();

			const firstUrl = first.value!.request.options.url!.toString();
			const firstHeaders = {...first.value!.request.options.headers};
			assert.equal(firstUrl, `${server.base}/iso?page=1`);
			assert.equal(firstHeaders['x-caller'], 'keep');

			await iterator.next();

			// Merging the successor did not rewrite the already delivered page.
			assert.equal(first.value!.request.options.url!.toString(), firstUrl);
			assert.deepEqual({...first.value!.request.options.headers}, firstHeaders);

			// The caller's own options object and the instance defaults are intact.
			assert.equal(JSON.stringify(callerOptions), callerSnapshot);
			assert.equal(client.defaults.options.url, undefined);
			assert.equal(client.defaults.options.headers['x-caller'], undefined);

			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('stops at requestLimit without calling paginate again', async () => {
		const server = startInProcessServer((request, response) => {
			const page = Number(new URL(request.url ?? '/', BASE_URL).searchParams.get('n') ?? 1);
			response.setHeader('link', `</limit?n=${page + 1}>; rel="next"`);
			json(response, {page});
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});
		let paginateCalls = 0;

		try {
			const iterator = client.paginate.pages(`${server.base}/limit?n=1`, undefined, {
				requestLimit: 2,
				paginate(response) {
					paginateCalls++;
					const reference = String(response.headers.link).match(/<([^>]+)>/)![1]!;
					return {url: new URL(reference, response.url)};
				},
			});

			const first = await iterator.next();
			assert.deepEqual(first.value!.body, {page: 1});
			const second = await iterator.next();
			assert.deepEqual(second.value!.body, {page: 2});

			assert.equal((await iterator.next()).done, true);
			assert.equal(urls(server).length, 2);
			// Only the first page needed a continuation decision.
			assert.equal(paginateCalls, 1);
		} finally {
			await server.close();
		}
	});

	await t.test('rejects a continuation `url` that is not a URL instance', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/u1') {
				response.setHeader('link', '</u2>; rel="next"');
				json(response, {page: 1});
			} else {
				json(response, {page: 2});
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/u1`, undefined, {
				paginate: () => ({url: '/u2' as unknown as URL}),
			});

			assert.deepEqual((await iterator.next()).value!.body, {page: 1});
			const error = await captureRejects(iterator.next());
			assert.ok(error instanceof TypeError);
			assert.match(error.message, /URL/);
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('never runs item level transform, filter or shouldContinue', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/h1') {
				response.setHeader('link', '</h2>; rel="next"');
				json(response, [1, 2]);
			} else {
				json(response, [3]);
			}
		});

		// The instance carries legacy pagination hooks that would misbehave if the
		// pages iterator ever borrowed the item-level pipeline.
		let transformCalls = 0;
		let filterCalls = 0;
		let shouldContinueCalls = 0;

		const client = got.extend({
			agent: {http: server.agent},
			responseType: 'json',
			retry: {limit: 0},
			pagination: {
				transform: () => {
					transformCalls++;
					return [];
				},
				filter: () => {
					filterCalls++;
					return true;
				},
				shouldContinue: () => {
					shouldContinueCalls++;
					return true;
				},
			},
		});

		try {
			const iterator = client.paginate.pages<number[]>(`${server.base}/h1`);
			const bodies: unknown[] = [];
			let result = await iterator.next();
			while (!result.done) {
				bodies.push(result.value!.body);
				result = await iterator.next();
			}

			assert.deepEqual(bodies, [[1, 2], [3]]);
			assert.equal(transformCalls, 0);
			assert.equal(filterCalls, 0);
			assert.equal(shouldContinueCalls, 0);
		} finally {
			await server.close();
		}
	});

	await t.test('counts a retried 503 as a single page request', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url !== '/flaky') {
				response.writeHead(404).end();
				return;
			}

			const attempts = server.requests.filter(candidate => candidate.url === '/flaky').length;
			if (attempts === 1) {
				response.writeHead(503).end();
				return;
			}

			json(response, {ok: true});
		});

		const client = got.extend({
			agent: {http: server.agent},
			responseType: 'json',
			retry: {
				limit: 2,
				// Keep the built-in retry chain, but without the ~1s backoff.
				calculateDelay: () => 5,
			},
		});

		try {
			const iterator = client.paginate.pages(`${server.base}/flaky`, undefined, {requestLimit: 1});
			const page = await iterator.next();
			assert.deepEqual(page.value!.body, {ok: true});
			// Two HTTP attempts (one 503, one success) but only one logical page.
			assert.equal(server.requests.filter(request => request.url === '/flaky').length, 2);
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});
});

test('page pipeline stop', async t => {
	await t.test('return() cancels a stalled in-flight request without waiting', async () => {
		const stallBarrier = barrier();
		const server = startInProcessServer((_request, response) => {
			response.writeHead(200, {'content-type': 'application/json'});
			response.write('{"partial":');
			stallBarrier.promise.then(() => response.end('}'));
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/stall`);
			const pending = iterator.next();

			await flush();
			assert.deepEqual(urls(server), ['/stall']);

			const returnedAt = Date.now();
			const stopped = await iterator.return();
			const elapsed = Date.now() - returnedAt;

			assert.deepEqual(stopped, {value: undefined, done: true});
			assert.ok(elapsed < 1000, `return() waited for the stalled read (${elapsed}ms)`);

			// The blocked `next()` settles done instead of rejecting.
			const nextResult = await pending;
			assert.equal(nextResult.done, true);

			// The underlying server-side connection was actually torn down.
			await flush();
			assert.equal(server.requests[0]?.closed, true);

			assert.equal((await iterator.next()).done, true);
		} finally {
			stallBarrier.release();
			await server.close();
		}
	});

	await t.test('return() during a long backoff does not wait for the timer', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/slow1') {
				response.setHeader('link', '</slow2>; rel="next"');
				json(response, {page: 1});
			} else {
				json(response, {page: 2});
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/slow1`, undefined, {backoff: 60_000});
			const first = await iterator.next();
			assert.deepEqual(first.value!.body, {page: 1});
			await flush();
			assert.deepEqual(urls(server), ['/slow1']);

			const returnedAt = Date.now();
			await iterator.return();
			assert.ok(Date.now() - returnedAt < 1000, 'return() waited for the backoff');

			// The successor request was never made.
			assert.deepEqual(urls(server), ['/slow1']);
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('return() interrupts a pending next parked in its backoff wait', async () => {
		// With prefetching disabled the successor task is created inside `next()`,
		// so a long backoff can be running while `next()` is still pending. A
		// concurrent `return()` must abort that wait instead of letting `next()`
		// sleep for the full backoff.
		const server = startInProcessServer((request, response) => {
			if (request.url === '/w1') {
				response.setHeader('link', '</w2>; rel="next"');
				json(response, {page: 1});
			} else {
				json(response, {page: 2});
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/w1`, undefined, {backoff: 60_000, prefetch: false});

			const first = await iterator.next();
			assert.deepEqual(first.value!.body, {page: 1});

			// This `next()` is parked in the backoff wait with no request in flight.
			const pending = iterator.next();
			await flush();
			assert.deepEqual(urls(server), ['/w1']);

			const returnedAt = Date.now();
			const stopped = await iterator.return();
			assert.ok(Date.now() - returnedAt < 1000, 'return() waited for the backoff');
			assert.deepEqual(stopped, {value: undefined, done: true});

			// The pending next must settle promptly as done, never rejecting.
			const nextResult = await pending;
			assert.equal(nextResult.done, true);
			assert.deepEqual(urls(server), ['/w1']);
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('external AbortSignal rejects the pending next once, then ends', async () => {
		const stallBarrier = barrier();
		const server = startInProcessServer((_request, response) => {
			response.writeHead(200, {'content-type': 'application/json'});
			response.write('{"partial":');
			stallBarrier.promise.then(() => response.end('}'));
		});

		const controller = new AbortController();
		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/abort-now`, {signal: controller.signal});
			const pending = iterator.next();
			await flush();

			controller.abort();

			const error = await captureRejects(pending);
			assert.equal(error.name, 'AbortError');
			assert.equal(error.code, 'ERR_ABORTED');
			assert.equal((await iterator.next()).done, true);
		} finally {
			stallBarrier.release();
			await server.close();
		}
	});

	await t.test('external abort with no pending next throws on the following next only once', async () => {
		const secondPageBarrier = barrier();
		const server = startInProcessServer((request, response) => {
			if (request.url === '/a1') {
				response.setHeader('link', '</a2>; rel="next"');
				json(response, {page: 1});
			} else {
				response.writeHead(200, {'content-type': 'application/json'});
				response.write('{"page":2');
				secondPageBarrier.promise.then(() => response.end('}'));
			}
		});

		const controller = new AbortController();
		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/a1`, {signal: controller.signal});
			const first = await iterator.next();
			assert.deepEqual(first.value!.body, {page: 1});
			await flush();

			// No `next()` is pending; the prefetched successor is in flight.
			controller.abort();
			await flush();

			await assert.rejects(iterator.next(), (error: Error) => error.name === 'AbortError');
			assert.equal((await iterator.next()).done, true);
		} finally {
			secondPageBarrier.release();
			await server.close();
		}
	});

	await t.test('external abort during a background backoff rejects with an AbortError and sends no request', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/bo1') {
				response.setHeader('link', '</bo2>; rel="next"');
				json(response, {page: 1});
			} else {
				json(response, {page: 2});
			}
		});

		const controller = new AbortController();
		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			// The prefetch task for page 2 parks in the backoff wait with no
			// request in flight when the external signal aborts.
			const iterator = client.paginate.pages(`${server.base}/bo1`, {signal: controller.signal}, {backoff: 60_000});
			const first = await iterator.next();
			assert.deepEqual(first.value!.body, {page: 1});
			await flush();
			assert.deepEqual(urls(server), ['/bo1']);

			controller.abort();

			const error = await captureRejects(iterator.next());
			assert.equal(error.name, 'AbortError');
			assert.equal(error.code, 'ERR_ABORTED');
			assert.ok(error.options, 'AbortError should expose the request options');
			assert.equal((await iterator.next()).done, true);

			// The second request was never made because the abort hit the wait.
			assert.deepEqual(urls(server), ['/bo1']);
		} finally {
			await server.close();
		}
	});

	await t.test('return before start sends nothing; repeated return stays successful', async () => {
		const server = startInProcessServer((_request, response) => json(response, {ok: true}));
		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/never`);
			assert.deepEqual(await iterator.return(), {value: undefined, done: true});
			assert.deepEqual(await iterator.return(), {value: undefined, done: true});
			assert.deepEqual(urls(server), []);
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('closing one iterator leaves the shared instance, agent and another iterator alone', async () => {
		const bStall = barrier();
		const server = startInProcessServer((request, response) => {
			if (request.url === '/a-stall') {
				response.writeHead(200, {'content-type': 'application/json'});
				response.write('{"a":1}');
				return;
			}

			if (request.url === '/b-stall') {
				response.writeHead(200, {'content-type': 'application/json'});
				response.write('{"b":');
				bStall.promise.then(() => response.end('}'));
				return;
			}

			json(response, {single: true});
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iteratorA = client.paginate.pages(`${server.base}/a-stall`);
			const iteratorB = client.paginate.pages(`${server.base}/b-stall`);

			const pendingA = iteratorA.next();
			const pendingB = iteratorB.next();
			await flush();
			assert.deepEqual(urls(server), ['/a-stall', '/b-stall']);

			await iteratorA.return();
			await pendingA;
			await flush();

			// Iterator B's connection is still open and its read still pending.
			const bRecord = server.requests.find(request => request.url === '/b-stall');
			assert.equal(bRecord?.closed, false);

			// A plain single request on the same instance keeps working.
			const single = await client(`${server.base}/single`);
			assert.deepEqual((single as Response).body, {single: true});

			// Iterator B can still be closed on its own; its blocked next then ends.
			await iteratorB.return();
			const bResult = await pendingB;
			assert.equal(bResult.done, true);
			assert.equal((await iteratorB.next()).done, true);
		} finally {
			bStall.release();
			await server.close();
		}
	});
});

test('page pipeline errors', async t => {
	await t.test('a prefetched HTTP error does not disturb the current page and is thrown once', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/ok') {
				response.setHeader('link', '</boom>; rel="next"');
				json(response, {page: 1});
			} else {
				response.writeHead(500).end('boom');
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/ok`);
			const first = await iterator.next();
			assert.deepEqual(first.value!.body, {page: 1});

			// Let the background successor fail.
			await flush();

			const error = await captureRejects(iterator.next());
			assert.equal(error.name, 'HTTPError');
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('a body parse error on the successor is delivered on the next call', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/p-ok') {
				response.setHeader('link', '</p-bad>; rel="next"');
				json(response, {page: 1});
			} else {
				response.setHeader('content-type', 'application/json');
				response.end('{not valid json');
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/p-ok`);
			assert.deepEqual((await iterator.next()).value!.body, {page: 1});
			await flush();

			const error = await captureRejects(iterator.next());
			assert.equal(error.name, 'ParseError');
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('a throwing paginate callback rejects the consuming next and ends the chain', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/c1') {
				response.setHeader('link', '</c2>; rel="next"');
				json(response, {page: 1});
				return;
			}

			json(response, {page: 2});
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});
		const callbackError = new Error('paginate exploded');

		try {
			const iterator = client.paginate.pages(`${server.base}/c1`, undefined, {
				paginate(response) {
					if (response.url.endsWith('/c2')) {
						throw callbackError;
					}

					return {url: new URL('/c2', response.url)};
				},
			});

			assert.deepEqual((await iterator.next()).value!.body, {page: 1});
			await flush();
			await assert.rejects(iterator.next(), error => error === callbackError);
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('returning early swallows an undelivered prefetched failure', async () => {
		const server = startInProcessServer((request, response) => {
			if (request.url === '/swallow1') {
				response.setHeader('link', '</swallow2>; rel="next"');
				json(response, {page: 1});
			} else {
				response.writeHead(500).end('boom');
			}
		});

		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		try {
			const iterator = client.paginate.pages(`${server.base}/swallow1`);
			assert.deepEqual((await iterator.next()).value!.body, {page: 1});
			await flush();

			// The failed successor is held but never consumed.
			await iterator.return();
			await flush();
			assert.equal((await iterator.next()).done, true);
		} finally {
			await server.close();
		}
	});

	await t.test('invalid page options throw synchronously before any request', async () => {
		const server = startInProcessServer((_request, response) => json(response, {ok: true}));
		const client = got.extend({agent: {http: server.agent}, responseType: 'json', retry: {limit: 0}});

		const invalid: Array<Record<string, unknown>> = [
			{requestLimit: 1.5},
			{requestLimit: Number.POSITIVE_INFINITY},
			{backoff: -1},
			{backoff: Number.NaN},
			{prefetch: 'yes'},
			{paginate: null},
		];

		try {
			for (const pageOptions of invalid) {
				assert.throws(() => client.paginate.pages(`${server.base}/x`, undefined, pageOptions as never), TypeError);
			}

			assert.deepEqual(urls(server), []);
		} finally {
			await server.close();
		}
	});
});
