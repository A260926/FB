import assert from 'node:assert/strict';
import http from 'node:http';
import {once} from 'node:events';
import {setImmediate as turn} from 'node:timers/promises';
import got from '../source/index.ts';

const hits = [];
let blockedSeen;
const blocked = new Promise(resolve => { blockedSeen = resolve; });
let blockedClosed;
const closed = new Promise(resolve => { blockedClosed = resolve; });
const server = http.createServer((request, response) => {
  hits.push(request.url);
  if (request.url === '/blocked') {
    response.on('close', blockedClosed);
    response.writeHead(200, {'content-type': 'application/json'});
    response.write('[');
    blockedSeen();
    return;
  }
  response.setHeader('content-type', 'application/json');
  if (request.url === '/first') {
    response.setHeader('link', '</empty?cursor=opaque%2B2>; rel="next"');
    response.end('[1,2]');
  } else if (request.url === '/empty?cursor=opaque%2B2') {
    response.setHeader('link', '</last>; rel="next"');
    response.end('[]');
  } else if (request.url === '/last') {
    response.end('[3]');
  } else {
    response.writeHead(404).end();
  }
});
server.listen(0, '127.0.0.1');
await once(server, 'listening');
const base = `http://127.0.0.1:${server.address().port}`;
try {
  const iterator = got.paginate(`${base}/first`, {retry: {limit: 0}});
  assert.deepEqual(hits, []);
  assert.equal((await iterator.next()).value, 1);
  await turn();
  assert.deepEqual(hits, ['/first']);
  assert.equal((await iterator.next()).value, 2);
  await turn();
  assert.deepEqual(hits, ['/first']);
  console.log('BASELINE_SERIAL_BOUNDARY first=[1,2] requestsBeforeResume=1');
  assert.equal((await iterator.next()).value, 3);
  assert.equal((await iterator.next()).done, true);
  assert.deepEqual(hits, ['/first', '/empty?cursor=opaque%2B2', '/last']);
  console.log('BASELINE_EMPTY_CONTINUES values=[1,2,3] requests=3');
  const pending = got(`${base}/blocked`, {retry: {limit: 0}});
  const cancellation = assert.rejects(pending, {name: 'CancelError', code: 'ERR_CANCELED'});
  await blocked;
  pending.cancel();
  await cancellation;
  await closed;
  console.log('BASELINE_CANCEL_OK partial response closed');
  console.log('BASELINE_OK');
} finally {
  server.closeAllConnections();
  server.close();
  await once(server, 'close');
}
