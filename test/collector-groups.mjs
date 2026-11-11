import assert from 'node:assert/strict';
import {test} from 'node:test';
import PQueue, {PriorityQueue} from '../source/index.ts';

const tick = () => new Promise(resolve => setImmediate(resolve));

// A promise controlled by the test, never by a timer. Tasks log when they start and when they are released.
const controlled = () => {
	let resolve;
	let reject;
	const promise = new Promise((res, rej) => {
		resolve = res;
		reject = rej;
	});
	return {promise, resolve, reject};
};

test('groups: blocked high-priority task lets a runnable lower-priority task take the global slot', async () => {
	// The scenario from the spec: concurrency 2, groupConcurrency 1.
	const queue = new PQueue({concurrency: 2, groupConcurrency: 1});
	const log = [];
	const a1 = controlled();
	const b1 = controlled();

	const a1Task = queue.add(
		() => {
			log.push('start:A1');
			return a1.promise.then(() => log.push('release:A1'));
		},
		{group: 'A'},
	);
	const a2Task = queue.add(
		() => {
			log.push('start:A2');
		},
		{group: 'A', priority: 9},
	);
	const b1Task = queue.add(
		() => {
			log.push('start:B1');
			return b1.promise;
		},
		{group: 'B', priority: 0},
	);

	await tick();

	// A1 and B1 are the first two starts. A2 (priority 9) is group-blocked and must not occupy a global slot waiting for A1.
	assert.deepEqual(log, ['start:A1', 'start:B1']);
	assert.equal(queue.pending, 2);
	assert.equal(queue.size, 1); // Only A2 waits.

	a1.resolve();
	await a1Task;
	await tick();

	// Only after A1 releases its group slot can A2 start, keeping its priority.
	assert.deepEqual(log, ['start:A1', 'start:B1', 'release:A1', 'start:A2']);
	b1.resolve();
	await Promise.all([a2Task, b1Task]);
	await queue.onIdle();
});

test('groups: the group cap is independent of the global concurrency', async () => {
	const queue = new PQueue({concurrency: 10, groupConcurrency: 2});
	const log = [];
	const gates = [controlled(), controlled(), controlled()];
	const yGate = controlled();

	for (const [index, gate] of gates.entries()) {
		queue.add(
			() => {
				log.push(`start:X${index}`);
				return gate.promise;
			},
			{group: 'X'},
		);
	}

	queue.add(() => {
		log.push('start:Y');
		return yGate.promise;
	}, {group: 'Y'});

	await tick();

	// Two X tasks run, the third X task is group-blocked, while Y runs under the global limit.
	assert.deepEqual(log, ['start:X0', 'start:X1', 'start:Y']);
	assert.equal(queue.pending, 3);
	assert.equal(queue.size, 1);

	gates[0].resolve();
	await tick();

	assert.deepEqual(log, ['start:X0', 'start:X1', 'start:Y', 'start:X2']);
	for (const gate of gates.slice(1)) {
		gate.resolve();
	}

	yGate.resolve();
	await queue.onIdle();
});

test('groups: an empty string is a valid group, and tasks without a group ignore the limit', async () => {
	const queue = new PQueue({concurrency: 10, groupConcurrency: 1});
	const log = [];
	const gate = controlled();

	queue.add(() => {
		log.push('empty-1');
		return gate.promise;
	}, {group: ''});
	queue.add(() => {
		log.push('empty-2');
	}, {group: ''});
	queue.add(() => {
		log.push('no-group');
	});

	await tick();

	assert.deepEqual(log, ['empty-1', 'no-group']);
	gate.resolve();
	await tick();
	assert.deepEqual(log, ['empty-1', 'no-group', 'empty-2']);
	await queue.onIdle();
});

test('groups: blocked tasks stay waiting, keep their order, and run FIFO when the group frees', async () => {
	const queue = new PQueue({concurrency: 2, groupConcurrency: 1});
	const log = [];
	const gate = controlled();
	const zGate = controlled();

	queue.add(() => {
		log.push('X1');
		return gate.promise;
	}, {group: 'X'});
	queue.add(() => {
		log.push('X2');
	}, {group: 'X'});
	queue.add(() => {
		log.push('X3');
	}, {group: 'X'});
	// Another group fills the second global slot.
	queue.add(() => {
		log.push('Z');
		return zGate.promise;
	}, {group: 'Z'});

	await tick();
	assert.deepEqual(log, ['X1', 'Z']);
	assert.equal(queue.pending, 2);
	assert.equal(queue.size, 2);

	gate.resolve();
	zGate.resolve();
	await queue.onIdle();

	// X2 and X3 keep their enqueue order after the group slot comes back.
	assert.deepEqual(log, ['X1', 'Z', 'X2', 'X3']);
});

test('groups: normal completion, failure, and a synchronous throw all release the group slot', async () => {
	const queue = new PQueue({concurrency: 1, groupConcurrency: 1});
	const order = [];

	await queue.add(() => {
		order.push('ok');
	}, {group: 'g'});
	await queue.add(() => {
		order.push('after-ok');
	}, {group: 'g'});

	await queue.add(() => {
		order.push('reject');
		return Promise.reject(new Error('boom'));
	}, {group: 'g'}).catch(() => {});
	await queue.add(() => {
		order.push('after-reject');
	}, {group: 'g'});

	await queue.add(() => {
		order.push('throw');
		throw new Error('sync');
	}, {group: 'g'}).catch(() => {});
	await queue.add(() => {
		order.push('after-throw');
	}, {group: 'g'});

	assert.deepEqual(order, ['ok', 'after-ok', 'reject', 'after-reject', 'throw', 'after-throw']);
	await queue.onIdle();
});

test('groups: cancelling a waiting task does not release another group slot', async () => {
	const queue = new PQueue({concurrency: 2, groupConcurrency: 1});
	const controller = new AbortController();
	const gate = controlled();
	const log = [];

	const running = queue.add(() => {
		log.push('running');
		return gate.promise;
	}, {group: 'g'});
	const cancelled = queue.add(() => {
		log.push('cancelled-ran');
	}, {group: 'g', signal: controller.signal});
	const waiting = queue.add(() => {
		log.push('waiting');
	}, {group: 'g'});

	await tick();
	assert.deepEqual(log, ['running']);

	controller.abort();
	await assert.rejects(cancelled);
	await tick();

	// The group still has its one slot taken by `running`, so `waiting` must not start.
	assert.deepEqual(log, ['running']);
	assert.equal(queue.size, 1);

	gate.resolve();
	await running;
	await waiting;
	assert.deepEqual(log, ['running', 'waiting']);
	await queue.onIdle();
});

test('groups: abort and timeout while running release the group slot at the settle point', async () => {
	// Running abort.
	{
		const queue = new PQueue({concurrency: 1, groupConcurrency: 1});
		const controller = new AbortController();
		const log = [];

		const first = queue.add(async ({signal}) => {
			log.push('first');
			await new Promise((_resolve, reject) => {
				signal.addEventListener('abort', () => reject(signal.reason), {once: true});
			});
		}, {group: 'g', signal: controller.signal});

		const second = queue.add(() => {
			log.push('second');
		}, {group: 'g'});

		await tick();
		assert.deepEqual(log, ['first']);
		controller.abort();
		await assert.rejects(first);
		await second;
		assert.deepEqual(log, ['first', 'second']);
		await queue.onIdle();
	}

	// Timeout.
	{
		const queue = new PQueue({concurrency: 1, groupConcurrency: 1});
		const log = [];

		const slow = queue.add(async ({signal}) => {
			log.push('slow');
			await new Promise((_resolve, reject) => {
				signal.addEventListener('abort', () => reject(signal.reason), {once: true});
			});
		}, {group: 'g', timeout: 20});

		const next = queue.add(() => {
			log.push('next');
		}, {group: 'g'});

		await assert.rejects(slow, {name: 'TimeoutError'});
		await next;
		assert.deepEqual(log, ['slow', 'next']);
		await queue.onIdle();
	}
});

test('groups: clear only removes waiting tasks and never zeroes a running group count', async () => {
	const queue = new PQueue({concurrency: 2, groupConcurrency: 1});
	const gate = controlled();
	let idleResolved = false;
	let emptyResolved = false;

	const running = queue.add(() => gate.promise, {group: 'g'});
	queue.add(() => 'backlog-1', {group: 'g'});
	queue.add(() => 'backlog-2', {group: 'g'});

	const idle = queue.onIdle().then(() => {
		idleResolved = true;
	});
	const empty = queue.onEmpty().then(() => {
		emptyResolved = true;
	});

	await tick();
	queue.clear();
	await tick();

	assert.equal(queue.size, 0);
	assert.equal(queue.pending, 1);
	assert.equal(idleResolved, false);
	assert.equal(emptyResolved, true); // The waiting backlog is gone, so the queue is empty.

	// A new task of the same group is still blocked by the running task.
	let started = false;
	const after = queue.add(() => {
		started = true;
	}, {group: 'g'});
	await tick();
	assert.equal(started, false);

	gate.resolve();
	await running;
	await after;
	await idle;
	assert.equal(started, true);
	assert.equal(idleResolved, true);
});

test('groups: onIdle does not resolve while a group is full and tasks are backlogged', async () => {
	const queue = new PQueue({concurrency: 1, groupConcurrency: 1});
	const gate = controlled();
	let idleResolved = false;

	const running = queue.add(() => gate.promise, {group: 'g'});
	queue.add(() => 'backlog', {group: 'g'});
	const idle = queue.onIdle().then(() => {
		idleResolved = true;
	});

	await tick();
	assert.equal(idleResolved, false);
	gate.resolve();
	await running;
	await idle;
	assert.equal(idleResolved, true);
});

test('groups: setPriority keeps the group key', async () => {
	const queue = new PQueue({concurrency: 1, groupConcurrency: 1, autoStart: false});
	const order = [];

	queue.add(() => order.push('a'), {group: 'g', id: 'a', priority: 1});
	queue.add(() => order.push('b'), {group: 'g', id: 'b', priority: 0});
	queue.setPriority('b', 5);

	queue.start();
	await queue.onIdle();
	assert.deepEqual(order, ['b', 'a']);
});

test('groups: duplicated task ids never share a group count', async () => {
	const queue = new PQueue({concurrency: 4, groupConcurrency: 1});
	const gate = controlled();
	const log = [];

	queue.add(() => {
		log.push('first');
		return gate.promise;
	}, {group: 'g', id: 'dup'});
	// Same id, same group: it must wait for a slot of its own, not borrow the first task's count.
	queue.add(() => {
		log.push('second');
	}, {group: 'g', id: 'dup'});
	// Same id, another group: independent slot, starts immediately.
	queue.add(() => {
		log.push('other-group');
	}, {group: 'h', id: 'dup'});

	await tick();
	assert.deepEqual(log, ['first', 'other-group']);

	gate.resolve();
	await tick();
	assert.deepEqual(log, ['first', 'other-group', 'second']);
	await queue.onIdle();
});

test('groups: addAll forwards the group option to every task', async () => {
	const queue = new PQueue({concurrency: 1, groupConcurrency: 1});
	const gate = controlled();
	const log = [];

	const results = queue.addAll(
		[
			() => {
				log.push('one');
				return gate.promise.then(() => 1);
			},
			() => {
				log.push('two');
				return 2;
			},
		],
		{group: 'g'},
	);

	await tick();
	assert.deepEqual(log, ['one']);
	gate.resolve();
	assert.deepEqual(await results, [1, 2]);
	assert.deepEqual(log, ['one', 'two']);
	await queue.onIdle();
});

test('groups: pause and start keep the group limits', async () => {
	const queue = new PQueue({concurrency: 2, groupConcurrency: 1, autoStart: false});
	const log = [];

	queue.add(() => log.push('g1'), {group: 'g'});
	queue.add(() => log.push('g2'), {group: 'g'});
	queue.add(() => log.push('h1'), {group: 'h'});

	await tick();
	assert.deepEqual(log, []);

	queue.start();
	await queue.onIdle();
	// g1 and h1 take the two global slots (different groups); g2 follows g1.
	assert.deepEqual(log, ['g1', 'h1', 'g2']);
});

test('groups: a group-blocked task does not consume the interval rate quota', async () => {
	const queue = new PQueue({
		concurrency: 3,
		groupConcurrency: 1,
		intervalCap: 2,
		interval: 40,
	});
	const log = [];
	const aGate = controlled();
	const bGate = controlled();
	const b2Started = controlled();

	queue.add(() => {
		log.push('A1');
		return aGate.promise;
	}, {group: 'A'});
	queue.add(() => {
		log.push('A2');
	}, {group: 'A'});
	queue.add(() => {
		log.push('A3');
	}, {group: 'A'});
	queue.add(() => {
		log.push('B1');
		return bGate.promise;
	}, {});
	queue.add(() => {
		log.push('B2');
		b2Started.resolve();
	}, {});

	await tick();

	// The first window has only two starts: A1 and B1. A2/A3 are group-blocked and B2 waits for the next window instead of an A task spending the quota.
	assert.deepEqual(log, ['A1', 'B1']);
	assert.equal(queue.pending, 2);
	assert.equal(queue.size, 3);
	assert.equal(queue.isRateLimited, true);

	// Deterministically wait for B2, which can only start in the next interval window.
	await b2Started.promise;
	assert.ok(log.includes('B2'));
	assert.ok(!log.includes('A2'), 'A2 must still be group-blocked when B2 starts');

	aGate.resolve();
	bGate.resolve();
	await queue.onIdle();
	assert.deepEqual(log, ['A1', 'B1', 'B2', 'A2', 'A3']);
});

test('groups: invalid groupConcurrency values are rejected', () => {
	for (const invalid of [0, -1, -Infinity, NaN, 1.5, 2.99]) {
		assert.throws(() => new PQueue({groupConcurrency: invalid}), {
			name: 'TypeError',
			message: /groupConcurrency/,
		});
	}

	// Valid values are accepted.
	new PQueue({groupConcurrency: 1});
	new PQueue({groupConcurrency: Number.MAX_SAFE_INTEGER});
	new PQueue({groupConcurrency: Infinity});
	// `undefined` falls back to the default.
	assert.equal(new PQueue({groupConcurrency: undefined}).size, 0);
});

test('groups: a finite groupConcurrency throws with a custom queueClass, but Infinity does not', () => {
	class FifoQueue extends PriorityQueue {}

	assert.throws(
		() => new PQueue({groupConcurrency: 1, queueClass: FifoQueue}),
		{name: 'TypeError', message: /groupConcurrency/},
	);

	// The default Infinity keeps custom queue classes working and ignores groups.
	const queue = new PQueue({groupConcurrency: Infinity, queueClass: FifoQueue, concurrency: 10});
	const gate = controlled();
	const log = [];
	queue.add(() => {
		log.push('one');
		return gate.promise;
	}, {group: 'g'});
	queue.add(() => {
		log.push('two');
	}, {group: 'g'});
	return tick().then(() => {
		assert.deepEqual(log, ['one', 'two']);
		gate.resolve();
		return queue.onIdle();
	});
});

test('groups: the default Infinity preserves the original behavior', async () => {
	const queue = new PQueue({concurrency: 10});
	const gate = controlled();
	const log = [];

	queue.add(() => {
		log.push(1);
		return gate.promise;
	}, {group: 'g'});
	queue.add(() => {
		log.push(2);
	}, {group: 'g'});
	queue.add(() => {
		log.push(3);
	}, {group: 'g'});

	await tick();
	assert.deepEqual(log, [1, 2, 3]);
	gate.resolve();
	await queue.onIdle();
});
