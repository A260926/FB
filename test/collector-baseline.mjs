import assert from 'node:assert/strict';
import {test} from 'node:test';
import PQueue from '../source/index.ts';
const tick = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => { let resolve; const promise = new Promise(r => {resolve = r;}); return {promise, resolve}; };

test('collector baseline: priority FIFO and no execution while paused', async () => {
  const q = new PQueue({concurrency: 1, autoStart: false});
  const order = [];
  const jobs = [['a',0], ['b',2], ['c',2]].map(([id,priority]) => q.add(() => order.push(id), {id, priority}));
  assert.equal(q.size, 3); assert.deepEqual(order, []);
  q.start(); await Promise.all(jobs); await q.onIdle();
  assert.deepEqual(order, ['b','c','a']); assert.equal(q.pending, 0);
});
test('collector baseline: queued cancellation and rejected running task release slots', async () => {
  const q = new PQueue({concurrency: 1}); const gate = deferred(); const ac = new AbortController();
  const first = q.add(() => gate.promise);
  const cancelled = q.add(() => {throw Error('must not start');}, {signal: ac.signal});
  const rejected = assert.rejects(cancelled, /cancelled/);
  ac.abort(Error('cancelled')); await rejected;
  assert.equal(q.size, 0); assert.equal(q.pending, 1);
  gate.resolve(); await first; await q.onIdle();
  await assert.rejects(q.add(() => {throw Error('fetch failed');}), /fetch failed/);
  assert.equal(await q.add(() => 'next page'), 'next page'); await q.onIdle();
});
test('collector baseline: blocked callback keeps onIdle pending', async () => {
  const q = new PQueue({concurrency: 2}); const gate = deferred(); let idle = false;
  const p = q.add(() => gate.promise); const done = q.onIdle().then(() => {idle=true;});
  await tick(); assert.equal(idle, false); assert.equal(q.pending, 1);
  gate.resolve(); await p; await done; assert.equal(idle, true);
});
