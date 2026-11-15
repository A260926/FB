'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const CDP = require('./index.js');
const {MemWSHub} = require('./support/mem-ws.cjs');
const {runFormAutomation} = require('./examples/form-automation.js');

const PROTOCOL = {
    domains: [
        {
            domain: 'Runtime',
            commands: [{name: 'evaluate'}, {name: 'enable'}],
            events: [{name: 'consoleAPICalled'}, {name: 'executionContextCreated'}],
        },
        {
            domain: 'Network',
            commands: [{name: 'enable'}],
            events: [
                {name: 'requestWillBeSent'},
                {name: 'loadingFinished'},
                {name: 'loadingFailed'},
            ],
        },
        {
            domain: 'Form',
            commands: [{name: 'read'}],
            events: [{name: 'changed'}],
        },
    ],
};

// scriptable in-process CDP endpoint: onCommand may push events before the
// default command response (which then acts as a barrier); returning false
// suppresses that response
function createHarness(onCommand) {
    const received = [];
    const hub = new MemWSHub((server) => {
        server.on('message', (raw) => {
            const message = JSON.parse(raw);
            received.push(message);
            const keep = onCommand ? onCommand(server, message) : true;
            if (keep !== false && message.id !== undefined) {
                server.send(JSON.stringify({
                    id: message.id,
                    result: {ok: true, echo: message.method},
                }));
            }
        });
    });
    return {hub, received};
}

async function connect(hub, protocol) {
    return await CDP({
        target: 'ws://local.test/devtools',
        protocol: protocol || PROTOCOL,
        createWebSocket: () => hub.createWebSocket(),
    });
}

async function drain(iterator, count) {
    const values = [];
    for (let i = 0; i < count; i++) {
        const result = await iterator.next();
        assert.equal(result.done, false);
        values.push(result.value);
    }
    return values;
}

// like assert.rejects but returns the rejection reason (assert.rejects itself
// resolves to undefined)
async function rejects(promise) {
    try {
        await promise;
    } catch (error) {
        return error;
    }
    throw new assert.AssertionError({message: 'Expected the promise to reject'});
}

function sendEvent(server, method, params, sessionId) {
    const message = {method, params};
    if (sessionId !== undefined) {
        message.sessionId = sessionId;
    }
    server.send(JSON.stringify(message));
}

// fixed protocol of the simulated form endpoint: one Runtime marker, one
// network request and its completion, all before the command response
function formOnCommand(expectFailureIds) {
    const failing = new Set(expectFailureIds || []);
    return (server, message) => {
        if (message.method !== 'Runtime.evaluate') {
            return true;
        }
        const {actionId} = message.params;
        const sid = message.sessionId;
        sendEvent(server, 'Runtime.consoleAPICalled', {
            type: 'log',
            args: [{value: actionId}, {value: message.params.expression}],
        }, sid);
        sendEvent(server, 'Network.requestWillBeSent', {
            requestId: `req-${actionId}`,
            request: {url: `https://form.test/${actionId}`},
            initiator: {actionId},
        }, sid);
        if (failing.has(actionId)) {
            sendEvent(server, 'Network.loadingFailed', {
                requestId: `req-${actionId}`,
                errorText: 'net::ERR_FAILED',
            }, sid);
        } else {
            sendEvent(server, 'Network.loadingFinished', {
                requestId: `req-${actionId}`,
                encodedDataLength: 42,
            }, sid);
        }
        return true;
    };
}

test('event-stream validation, ordering, sessions and multiple waiters', async (t) => {
    const harness = createHarness((server, message) => {
        if (message.method === 'Form.read') {
            // events are delivered before the command response
            sendEvent(server, 'Form.changed', {v: 1});
            sendEvent(server, 'Form.changed', {v: 2}, 'childA');
            sendEvent(server, 'Form.changed', {v: 3}, 'childB');
            sendEvent(server, 'Runtime.executionContextCreated', {v: 4});
            sendEvent(server, 'Form.changed', {v: 5});
        }
        return true;
    });
    const client = await connect(harness.hub);
    t.after(async () => {
        await client.close();
        harness.hub.close();
    });

    await t.test('validates arguments synchronously and leaves no subscription', () => {
        const badMethods = [undefined, null, [], '', 'Form.changed', 42, ['ok', 3]];
        for (const methods of badMethods) {
            assert.throws(() => client.observe(methods, {capacity: 4}), TypeError);
        }
        const badOptions = [
            undefined, null, {}, {capacity: 0}, {capacity: -1},
            {capacity: 1.5}, {capacity: '8'}, {capacity: 4, sessionId: 7},
            {capacity: 4, sessionId: ''}, {capacity: 4, signal: 'nope'},
        ];
        for (const options of badOptions) {
            assert.throws(() => client.observe(['Form.changed'], options), TypeError);
        }
        assert.equal(client._eventObservers.size, 0);
    });

    await t.test('deduplicates method names', async () => {
        const iterator = client.observe(
            ['Form.changed', 'Form.changed', 'Form.changed'],
            {capacity: 8}
        );
        await client.Form.read();
        const values = await drain(iterator, 4);
        assert.deepEqual(values.map((e) => e.params.v), [1, 2, 3, 5]);
        await iterator.return();
    });

    await t.test('collects events created before the first next() in arrival order', async () => {
        const iterator = client.observe(['Form.changed'], {capacity: 8});
        await client.Form.read(); // barrier: every event is already queued
        const values = await drain(iterator, 4);
        assert.deepEqual(values.map((e) => e.params.v), [1, 2, 3, 5]);
        assert.deepEqual(values.map((e) => e.sessionId), [undefined, 'childA', 'childB', undefined]);
        assert.deepEqual(Object.keys(values[0]), ['method', 'params']);
        await iterator.return();
    });

    await t.test('filters by sessionId: omitted, null and explicit flattened sessions', async () => {
        const all = client.observe(['Form.changed'], {capacity: 8});
        const rootOnly = client.observe(['Form.changed'], {capacity: 8, sessionId: null});
        const onlyA = client.observe(['Form.changed'], {capacity: 8, sessionId: 'childA'});
        const onlyB = client.observe(['Form.changed'], {capacity: 8, sessionId: 'childB'});
        await client.Form.read();
        assert.deepEqual((await drain(all, 4)).map((e) => e.params.v), [1, 2, 3, 5]);
        assert.deepEqual((await drain(rootOnly, 2)).map((e) => e.params.v), [1, 5]);
        assert.deepEqual((await drain(onlyA, 1)).map((e) => e.sessionId), ['childA']);
        assert.deepEqual((await drain(onlyB, 1)).map((e) => e.sessionId), ['childB']);
        await all.return();
        await rootOnly.return();
        await onlyA.return();
        await onlyB.return();
    });

    await t.test('delivers queued events to FIFO waiters exactly once', async () => {
        const iterator = client.observe(['Form.changed'], {capacity: 8});
        const w1 = iterator.next();
        const w2 = iterator.next();
        const w3 = iterator.next();
        await client.Form.read();
        const [r1, r2, r3] = await Promise.all([w1, w2, w3]);
        assert.deepEqual([r1.value.params.v, r2.value.params.v, r3.value.params.v], [1, 2, 3]);
        // nothing was delivered twice: the fourth matching event is still there
        const next = await iterator.next();
        assert.equal(next.value.params.v, 5);
        // a pending waiter is resolved by return() as done
        const pending = iterator.next();
        const done = await iterator.return();
        assert.equal(done.done, true);
        assert.equal((await pending).done, true);
        assert.equal((await iterator.next()).done, true);
    });

    await t.test('never sends enable commands by itself', () => {
        const enables = harness.received.filter((m) => m.method.endsWith('.enable'));
        assert.equal(enables.length, 0);
    });
});

test('event-stream object isolation and legacy emission order', async (t) => {
    let replyId = 0;
    const harness = createHarness((server, message) => {
        sendEvent(server, 'Form.changed', {nested: {value: 'orig-1'}, arr: [1, 2]});
        sendEvent(server, 'Form.changed', {nested: {value: 'orig-2'}, arr: [3, 4]});
        // reply after the events, acting as a barrier; suppress the default one
        server.send(JSON.stringify({id: message.id, result: {}}));
        return false;
    });
    const client = await connect(harness.hub);
    t.after(async () => {
        await client.close();
        harness.hub.close();
    });

    // the legacy listeners mutate the payload in place
    const legacyOrder = [];
    client.on('event', (message) => {
        legacyOrder.push('event');
        message.params.nested.value = 'mutated';
        message.params.arr.push(99);
    });
    client.on('Form.changed', (params) => {
        legacyOrder.push('named');
        params.arr.length = 0;
    });
    client.on('Form.changed.undefined', () => {
        legacyOrder.push('session');
    });

    const first = client.observe(['Form.changed'], {capacity: 8});
    const second = client.observe(['Form.changed'], {capacity: 8});

    // barrier: wait for the command response sent after both events
    await client.send('Form.read');

    const firstEvents = await drain(first, 2);
    const secondEvents = await drain(second, 2);
    await first.return();
    await second.return();

    await t.test('queued snapshots are immune to legacy in-place mutations', () => {
        assert.deepEqual(firstEvents[0].params, {nested: {value: 'orig-1'}, arr: [1, 2]});
        assert.deepEqual(firstEvents[1].params, {nested: {value: 'orig-2'}, arr: [3, 4]});
        assert.deepEqual(secondEvents[0].params, {nested: {value: 'orig-1'}, arr: [1, 2]});
    });

    await t.test('consumers cannot mutate each other or the source message', () => {
        secondEvents[0].params.nested.value = 'changed-by-second';
        secondEvents[0].params.arr.push(7);
        assert.equal(firstEvents[0].params.nested.value, 'orig-1');
        assert.deepEqual(firstEvents[0].params.arr, [1, 2]);
        assert.notEqual(firstEvents[0].params, secondEvents[0].params);
        assert.notEqual(firstEvents[0].params.nested, secondEvents[0].params.nested);
    });

    await t.test('keeps the legacy event/named/session emission order', () => {
        assert.deepEqual(legacyOrder, ['event', 'named', 'session', 'event', 'named', 'session']);
    });
});

test('event-stream overflow terminates without dropping queued events', async (t) => {
    const harness = createHarness((server) => {
        sendEvent(server, 'Form.changed', {v: 'e1'});
        sendEvent(server, 'Form.changed', {v: 'e2'});
        sendEvent(server, 'Form.changed', {v: 'e3'});
    });
    const client = await connect(harness.hub);
    t.after(async () => {
        await client.close();
        harness.hub.close();
    });

    await t.test('full queue overflows: queued events stay readable, then it rejects', async () => {
        const iterator = client.observe(['Form.changed'], {capacity: 1});
        await client.Form.read(); // barrier: three events arrived in one burst
        // the first event is retained, the second one triggered the overflow
        const queued = await iterator.next();
        assert.equal(queued.value.params.v, 'e1');
        const firstError = await rejects(iterator.next());
        assert.equal(firstError.code, 'ERR_CDP_EVENT_OVERFLOW');
        assert.equal(firstError.capacity, 1);
        assert.equal(firstError.method, 'Form.changed');
        // reentrant next() from the rejection keeps rejecting with the same error
        const secondError = await rejects(iterator.next());
        assert.equal(secondError, firstError);
    });

    await t.test('other observers, legacy listeners and commands keep working', async () => {
        const legacy = [];
        client.on('Form.changed', (params) => legacy.push(params.v));
        const slow = client.observe(['Form.changed'], {capacity: 1});
        const healthy = client.observe(['Form.changed'], {capacity: 8});
        await client.Form.read();
        // the healthy iterator sees the whole burst including the trigger event
        const healthyEvents = await drain(healthy, 3);
        assert.deepEqual(healthyEvents.map((e) => e.params.v), ['e1', 'e2', 'e3']);
        await healthy.return();
        assert.deepEqual(legacy, ['e1', 'e2', 'e3']);
        // the slow one still exposes its single queued event then fails
        assert.equal((await slow.next()).value.params.v, 'e1');
        await assert.rejects(slow.next(), {code: 'ERR_CDP_EVENT_OVERFLOW'});
        // ordinary commands still complete after the observer failure
        const reply = await client.Form.read();
        assert.equal(reply.ok, true);
        assert.equal(client._eventObservers.size, 0);
    });
});

test('event-stream termination: return, abort, close, disconnect', async (t) => {
    await t.test('return() clears the queue and survives later abort/disconnect', async () => {
        const harness = createHarness((server) => {
            sendEvent(server, 'Form.changed', {v: 1});
        });
        const client = await connect(harness.hub);
        const iterator = client.observe(['Form.changed'], {capacity: 4});
        const controller = new AbortController();
        const tracked = client.observe(['Form.changed'], {capacity: 4, signal: controller.signal});
        await client.Form.read();
        await iterator.return();
        await tracked.return();
        assert.equal((await iterator.next()).done, true);
        // terminal events after return must not affect the finished iterators
        controller.abort('late abort');
        harness.hub.disconnect();
        assert.equal((await iterator.next()).done, true);
        assert.equal((await tracked.next()).done, true);
        assert.equal(client._eventObservers.size, 0);
        harness.hub.close();
    });

    await t.test('abort clears the queue and rejects waiters with signal.reason', async () => {
        const harness = createHarness((server) => {
            sendEvent(server, 'Form.changed', {v: 1});
            sendEvent(server, 'Form.changed', {v: 2});
        });
        const client = await connect(harness.hub);
        const controller = new AbortController();
        const iterator = client.observe(['Form.changed'], {capacity: 4, signal: controller.signal});
        const pending = iterator.next();
        await client.Form.read(); // one event resolves the waiter, the other is queued
        const consumed = await pending;
        assert.equal(consumed.value.params.v, 1);
        const reason = new Error('flow cancelled');
        controller.abort(reason);
        // the queued event is dropped: next() rejects instead of returning it
        await assert.rejects(iterator.next(), (error) => error === reason);
        await assert.rejects(iterator.next(), (error) => error === reason);
        // the connection itself is untouched: commands still complete
        const reply = await client.Form.read();
        assert.equal(reply.ok, true);
        assert.equal(client._eventObservers.size, 0);
        // a later disconnect does not replace the stored reason
        harness.hub.disconnect();
        await assert.rejects(iterator.next(), (error) => error === reason);
        harness.hub.close();
    });

    await t.test('an observer created after close terminates immediately', async () => {
        const harness = createHarness();
        const client = await connect(harness.hub);
        await client.close();
        const iterator = client.observe(['Form.changed'], {capacity: 4});
        const error = await rejects(iterator.next());
        assert.equal(error.code, 'ERR_CDP_EVENT_CONNECTION_CLOSED');
        assert.equal(client._eventObservers.size, 0);
        harness.hub.close();
    });

    await t.test('removes the abort listener when finished (tracked signal)', async () => {
        const harness = createHarness();
        const client = await connect(harness.hub);
        const signal = {
            aborted: false,
            reason: 'boom',
            added: 0,
            removed: 0,
            addEventListener() { this.added++; },
            removeEventListener() { this.removed++; },
        };
        const iterator = client.observe(['Form.changed'], {capacity: 2, signal});
        assert.equal(signal.added, 1);
        await iterator.return();
        assert.equal(signal.removed, 1);
        await client.close();
        harness.hub.close();
    });

    await t.test('remote disconnect drains the queue then rejects connection closed', async () => {
        const harness = createHarness((server) => {
            sendEvent(server, 'Form.changed', {v: 'before-drop'});
        });
        const client = await connect(harness.hub);
        const iterator = client.observe(['Form.changed'], {capacity: 4});
        // a separate observer on an event this harness never emits, so its
        // next() stays genuinely pending until the connection drops
        const waiting = client.observe(['Runtime.executionContextCreated'], {capacity: 4});
        const pending = waiting.next();
        const messageListeners = client._ws.listenerCount('message');
        await client.Form.read();
        harness.hub.disconnect();
        // queued events remain readable first
        assert.equal((await iterator.next()).value.params.v, 'before-drop');
        const error = await rejects(iterator.next());
        assert.equal(error.code, 'ERR_CDP_EVENT_CONNECTION_CLOSED');
        const pendingError = await rejects(pending);
        assert.equal(pendingError.code, 'ERR_CDP_EVENT_CONNECTION_CLOSED');
        await rejects(iterator.next());
        assert.equal(client._eventObservers.size, 0);
        assert.equal(client._ws.listenerCount('message'), messageListeners);
        harness.hub.close();
    });

    await t.test('client.close() terminates observers even without disconnect event', async () => {
        const harness = createHarness((server) => {
            sendEvent(server, 'Form.changed', {v: 'queued'});
        });
        const client = await connect(harness.hub);
        const withQueue = client.observe(['Form.changed'], {capacity: 4});
        const empty = client.observe(['Form.changed'], {capacity: 4});
        await client.Form.read();
        await client.close();
        // every iterator owns its queue: both can drain the queued event first
        assert.equal((await withQueue.next()).value.params.v, 'queued');
        assert.equal((await empty.next()).value.params.v, 'queued');
        await assert.rejects(withQueue.next(), {code: 'ERR_CDP_EVENT_CONNECTION_CLOSED'});
        await assert.rejects(empty.next(), {code: 'ERR_CDP_EVENT_CONNECTION_CLOSED'});
        assert.equal(client._eventObservers.size, 0);
        // active close tears the WebSocket listeners down
        assert.equal(client._ws.listenerCount('message'), 0);
        harness.hub.close();
    });
});

test('event-stream form automation: records, ownership and ordering', async (t) => {
    const harness = createHarness(formOnCommand(['a-a-fail']));
    const client = await connect(harness.hub);
    t.after(async () => {
        await client.close();
        harness.hub.close();
    });

    const actions = [
        {actionId: 'r-root-ok', sessionId: undefined, expression: "root.fill('name')"},
        {actionId: 'a-a-ok', sessionId: 'childA', expression: "frameA.fill('email')"},
        {actionId: 'b-b-ok', sessionId: 'childB', expression: "frameB.fill('city')"},
        {actionId: 'a-a-fail', sessionId: 'childA', expression: "frameA.submit()", expectFailure: true},
    ];
    const {records, error} = await runFormAutomation(client, {actions, capacity: 16});
    assert.equal(error, null);

    await t.test('sends one Runtime.evaluate per action, no enable commands', () => {
        const evaluates = harness.received.filter((m) => m.method === 'Runtime.evaluate');
        assert.equal(evaluates.length, actions.length);
        assert.deepEqual(evaluates.map((m) => m.params.actionId), actions.map((a) => a.actionId));
        assert.deepEqual(evaluates.map((m) => m.sessionId), [undefined, 'childA', 'childB', 'childA']);
        assert.equal(harness.received.filter((m) => m.method.endsWith('.enable')).length, 0);
    });

    await t.test('merges intermediate and completion events into one record', () => {
        assert.equal(records.length, 4); // 12 events -> 4 records, not 12
        const byId = new Map(records.map((r) => [r.actionId, r]));
        for (const action of actions.slice(0, 3)) {
            const record = byId.get(action.actionId);
            assert.equal(record.status, 'completed');
            assert.equal(record.requestId, `req-${action.actionId}`);
            assert.equal(record.sessionId, action.sessionId === undefined ? null : action.sessionId);
            assert.equal(record.runtimeEvents.length, 1);
            assert.equal(record.runtimeEvents[0].sessionId, record.sessionId);
            assert.equal(record.failureReason, null);
        }
        const failed = byId.get('a-a-fail');
        assert.equal(failed.status, 'failed');
        assert.equal(failed.sessionId, 'childA');
        assert.equal(failed.failureReason, 'net::ERR_FAILED');
        assert.equal(failed.requestId, 'req-a-a-fail');
    });

    await t.test('publishes records in network completion order', () => {
        assert.deepEqual(records.map((r) => r.actionId),
            ['r-root-ok', 'a-a-ok', 'b-b-ok', 'a-a-fail']);
        assert.deepEqual(records.map((r) => r.status),
            ['completed', 'completed', 'completed', 'failed']);
    });

    await t.test('keeps every event attributed to its own flattened session', () => {
        const childA = records.filter((r) => r.sessionId === 'childA');
        const childB = records.filter((r) => r.sessionId === 'childB');
        const root = records.filter((r) => r.sessionId === null);
        assert.deepEqual(childA.map((r) => r.actionId), ['a-a-ok', 'a-a-fail']);
        assert.deepEqual(childB.map((r) => r.actionId), ['b-b-ok']);
        assert.deepEqual(root.map((r) => r.actionId), ['r-root-ok']);
        assert.equal(new Set(records.map((r) => r.requestId)).size, 4);
    });
});

test('event-stream form automation: overflow, disconnect and cancellation', async (t) => {
    await t.test('slow consumer overflow: current action incomplete, sibling record intact', async () => {
        const harness = createHarness((server, message) => {
            if (message.method !== 'Runtime.evaluate') {
                return true;
            }
            const {actionId} = message.params;
            const sid = message.sessionId;
            if (actionId === 'a1') {
                // a burst larger than the observer can retain while the
                // consumer holds a single next(): capacity 3 + 2 extra events
                sendEvent(server, 'Runtime.consoleAPICalled', {type: 'log', args: [{value: actionId}]}, sid);
                sendEvent(server, 'Network.requestWillBeSent', {
                    requestId: `req-${actionId}`, initiator: {actionId},
                }, sid);
                sendEvent(server, 'Runtime.consoleAPICalled', {type: 'log', args: [{value: actionId}]}, sid);
                sendEvent(server, 'Runtime.consoleAPICalled', {type: 'log', args: [{value: actionId}]}, sid);
                sendEvent(server, 'Network.loadingFinished', {requestId: `req-${actionId}`}, sid);
            } else {
                formOnCommand()(server, message);
                // keep the default command reply as the barrier
            }
            return true;
        });
        const client = await connect(harness.hub);
        const actions = [
            {actionId: 'r1', sessionId: undefined, expression: 'r1'},
            {actionId: 'a1', sessionId: 'childA', expression: 'a1'},
            {actionId: 'b1', sessionId: 'childB', expression: 'b1'},
        ];
        const {records, error} = await runFormAutomation(client, {actions, capacity: 3});
        assert.equal(error.code, 'ERR_CDP_EVENT_OVERFLOW');
        const byId = new Map(records.map((r) => [r.actionId, r]));
        // every input action yields exactly one record
        assert.deepEqual(records.map((r) => r.actionId), ['r1', 'a1', 'b1']);
        assert.equal(byId.get('r1').status, 'completed'); // earlier root record untouched
        assert.equal(byId.get('a1').status, 'incomplete');
        assert.equal(byId.get('a1').failureReason, 'overflow');
        assert.equal(byId.get('b1').status, 'incomplete'); // never started
        assert.equal(byId.get('b1').failureReason, 'overflow');
        assert.equal(records.filter((r) => r.status === 'completed').length, 1);
        assert.equal(records.filter((r) => r.status === 'incomplete').length, 2);
        // ordinary commands still work after the observer died
        const reply = await client.send('Form.read');
        assert.equal(reply.ok, true);
        await client.close();
        harness.hub.close();
    });

    await t.test('page disconnect: in-flight action incomplete, earlier records stand', async () => {
        const harness = createHarness((server, message) => {
            if (message.method === 'Runtime.evaluate' && message.params.actionId === 'a1') {
                // the page vanishes while the action is in flight, no reply
                harness.hub.disconnect(server);
                return false;
            }
            formOnCommand()(server, message);
            // keep the default command reply as the barrier
            return true;
        });
        const client = await connect(harness.hub);
        const actions = [
            {actionId: 'r1', sessionId: undefined, expression: 'r1'},
            {actionId: 'a1', sessionId: 'childA', expression: 'a1'},
            {actionId: 'b1', sessionId: 'childB', expression: 'b1'},
        ];
        const {records, error} = await runFormAutomation(client, {actions, capacity: 16});
        assert.equal(error.code, 'ERR_CDP_EVENT_CONNECTION_CLOSED');
        const byId = new Map(records.map((r) => [r.actionId, r]));
        assert.deepEqual(records.map((r) => r.actionId), ['r1', 'a1', 'b1']);
        assert.equal(byId.get('r1').status, 'completed');
        assert.equal(byId.get('a1').status, 'incomplete');
        assert.equal(byId.get('a1').failureReason, 'command-failed');
        assert.equal(byId.get('b1').status, 'incomplete'); // never started
        assert.equal(byId.get('b1').failureReason, 'disconnected');
        assert.equal(records.filter((r) => r.status === 'completed').length, 1);
        // only r1 and the interrupted a1 were sent
        const evaluates = harness.received.filter((m) => m.method === 'Runtime.evaluate');
        assert.deepEqual(evaluates.map((m) => m.params.actionId), ['r1', 'a1']);
        harness.hub.close();
    });

    await t.test('cancellation: current action incomplete and no success is published', async () => {
        const harness = createHarness(formOnCommand());
        const client = await connect(harness.hub);
        const controller = new AbortController();
        // cancel as soon as the second action's request is seen on the wire
        client.on('Network.requestWillBeSent', (params) => {
            if (params.requestId === 'req-a1') {
                controller.abort('user cancelled');
            }
        });
        const actions = [
            {actionId: 'r1', sessionId: undefined, expression: 'r1'},
            {actionId: 'a1', sessionId: 'childA', expression: 'a1'},
            {actionId: 'b1', sessionId: 'childB', expression: 'b1'},
        ];
        const {records, error} = await runFormAutomation(client, {
            actions,
            capacity: 16,
            signal: controller.signal,
        });
        assert.equal(error, 'user cancelled');
        const byId = new Map(records.map((r) => [r.actionId, r]));
        assert.deepEqual(records.map((r) => r.actionId), ['r1', 'a1', 'b1']);
        assert.equal(byId.get('r1').status, 'completed');
        assert.equal(byId.get('a1').status, 'incomplete');
        assert.equal(byId.get('a1').failureReason, 'aborted');
        assert.equal(byId.get('b1').status, 'incomplete'); // never started
        assert.equal(byId.get('b1').failureReason, 'aborted');
        // r1 and a1 were sent, b1 was not; the connection still works
        const evaluates = harness.received.filter((m) => m.method === 'Runtime.evaluate');
        assert.deepEqual(evaluates.map((m) => m.params.actionId), ['r1', 'a1']);
        const reply = await client.send('Form.read');
        assert.equal(reply.ok, true);
        await client.close();
        harness.hub.close();
    });
});
