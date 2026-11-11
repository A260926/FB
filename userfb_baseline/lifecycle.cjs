'use strict';
// Observe the unmodified crawler's request lifecycle. These assertions describe
// the starting behavior and are intentionally separate from future regressions.
const assert = require('node:assert/strict');
const http = require('node:http');
const {once} = require('node:events');
const zlib = require('node:zlib');
const Crawler = require('../');
const turn = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return {promise, resolve}; };
async function scenario(name, routes, run) {
    const sockets = new Set();
    const server = http.createServer(routes);
    server.on('connection', socket => { sockets.add(socket); socket.on('close', () => sockets.delete(socket)); });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const crawler = new Crawler(`http://127.0.0.1:${server.address().port}/${name}`);
    crawler.interval = 2;
    crawler.timeout = 60000;
    crawler.respectRobotsTxt = false;
    crawler.discoverResources = false;
    const guard = setTimeout(() => { console.error(`baseline stalled: ${name}`); process.exit(2); }, 5000);
    try { await run(crawler); } finally {
        clearTimeout(guard);
        crawler.stop(true);
        for (const socket of sockets) socket.destroy();
        await new Promise(resolve => server.close(resolve));
        crawler.httpAgent.destroy();
        crawler.httpsAgent.destroy();
    }
}
(async () => {
    await scenario('normal', (req, res) => res.end('ordinary page'), async crawler => {
        let delivered;
        crawler.on('fetchcomplete', (item, body) => { delivered = body.toString(); });
        const done = once(crawler, 'complete'); crawler.start(); await done;
        assert.equal(delivered, 'ordinary page');
        assert.equal(crawler.queue[0].status, 'downloaded');
        assert.equal(crawler._openRequests.length, 0);
        console.log('NORMAL_OK body=ordinary page status=downloaded open=0');
    });
    await scenario('oversize', (req, res) => {
        res.writeHead(200, {'Content-Type': 'text/html', 'Transfer-Encoding': 'chunked'});
        res.write('x'.repeat(32)); res.end('y'.repeat(32));
    }, async crawler => {
        crawler.maxResourceSize = 8;
        let published = 0, bodyLength; crawler.on('fetchcomplete', (item, body) => { published++; bodyLength = body.length; });
        const error = once(crawler, 'fetchdataerror'); crawler.start(); const [item] = await error; await turn();
        assert.equal(item.fetched, true); assert.equal(item.status, 'downloaded');
        assert.equal(crawler._openRequests.length, 0); assert.equal(published, 1); assert.equal(bodyLength, 0);
        console.log('OVERSIZE_FALSE_SUCCESS error=fetchdataerror status=downloaded published=1 body_bytes=0');
    });
    await scenario('condition', (req, res) => res.end('body'), async crawler => {
        const expected = new Error('condition backend unavailable');
        crawler.addDownloadCondition((item, response, callback) => queueMicrotask(() => callback(expected)));
        const error = once(crawler, 'downloadconditionerror'); crawler.start(); const [item, actual] = await error; await turn();
        assert.equal(actual, expected); assert.equal(item.fetched, false);
        assert.equal(crawler._openRequests.length, 1);
        console.log(`CONDITION_STUCK fetched=false status=${item.status} open=1 error_identity=true`);
    });
    await scenario('truncated', (req, res) => {
        res.writeHead(200, {'Content-Type': 'text/html', 'Content-Length': '100'});
        const socket = res.socket; res.end('partial'); socket.end();
    }, async crawler => {
        const aborted = deferred(); let published = 0;
        crawler.on('fetchcomplete', () => published++);
        crawler.on('fetchheaders', (item, response) => {
            response.on('error', () => {}); // Observation only: avoid process-level error handling.
            response.once('aborted', () => aborted.resolve(item));
        });
        crawler.start(); const item = await aborted.promise; await turn();
        assert.equal(item.fetched, false); assert.equal(item.status, 'headers');
        assert.equal(crawler._openRequests.length, 1); assert.equal(published, 0);
        console.log('TRUNCATED_STUCK fetched=false status=headers open=1 published=0');
    });
    await scenario('late', (req, res) => {
        res.writeHead(200, {'Content-Type': 'text/html'}); res.flushHeaders();
        // Keep both real responses open. Progress is controlled by barriers below.
    }, async crawler => {
        crawler.maxConcurrency = 2;
        crawler.queueURL('/survivor');
        const held = deferred(), survivorHeaders = deferred(); let release;
        crawler.addDownloadCondition((item, response, callback) => {
            if (item.path === '/late') { release = callback; held.resolve(item); }
            else callback(null, true);
        });
        crawler.on('fetchheaders', item => { if (item.path === '/survivor') survivorHeaders.resolve(item); });
        crawler.start(); const item = await held.promise; const survivor = await survivorHeaders.promise;
        assert.equal(crawler._openRequests.length, 2);
        const request = crawler._openRequests.find(req => req.path === '/late');
        const other = crawler._openRequests.find(req => req.path === '/survivor');
        const timedout = once(crawler, 'fetchtimeout');
        request.emit('timeout'); // Fire the real registered timeout handler, without a timing race.
        await timedout;
        assert.equal(item.status, 'timeout'); assert.ok(crawler._openRequests.includes(other));
        const prevented = once(crawler, 'downloadprevented'); release(null, false); await prevented; await turn();
        assert.equal(item.status, 'downloadprevented'); assert.equal(item.fetched, true);
        assert.equal(survivor.fetched, false); assert.equal(other.destroyed, false);
        assert.equal(crawler._openRequests.includes(other), false); assert.equal(crawler._openRequests.length, 0);
        console.log('LATE_CONDITION_CORRUPTS timeout->downloadprevented survivor_fetched=false survivor_socket_open=true tracked=0');
    });
    const zip = zlib.gzipSync(Buffer.from('decoded page'));
    const originalUnzip = zlib.unzip;
    const decoded = deferred(); let releaseUnzip;
    zlib.unzip = (buffer, callback) => originalUnzip(buffer, (...args) => {
        releaseUnzip = () => callback(...args); decoded.resolve();
    });
    try {
        await scenario('decompress', (req, res) => {
            if (req.url === '/decompress') {
                res.writeHead(200, {'Content-Type': 'text/html', 'Content-Encoding': 'gzip'}); res.end(zip);
            } else { res.writeHead(200, {'Content-Type': 'text/html'}); res.flushHeaders(); }
        }, async crawler => {
            crawler.maxConcurrency = 2; crawler.queueURL('/survivor');
            const survivorHeaders = deferred(); let cached = 0, published = 0;
            crawler.cache = {setCacheData() { cached++; }};
            crawler.on('fetchcomplete', () => published++);
            crawler.on('fetchheaders', item => { if (item.path === '/survivor') survivorHeaders.resolve(); });
            crawler.start(); await decoded.promise; await survivorHeaders.promise;
            assert.equal(cached, 1); // Cache publication already happened before processing finished.
            const request = crawler._openRequests.find(req => req.path === '/decompress');
            const other = crawler._openRequests.find(req => req.path === '/survivor');
            const timedout = once(crawler, 'fetchtimeout'); request.emit('timeout'); await timedout;
            const complete = once(crawler, 'fetchcomplete'); releaseUnzip(); const [item, body] = await complete;
            assert.equal(item.status, 'downloaded'); assert.equal(body.toString(), 'decoded page');
            assert.equal(published, 1); assert.equal(other.destroyed, false);
            assert.equal(crawler._openRequests.includes(other), false);
            console.log('LATE_DECODE_CORRUPTS cache_before_decode=1 timeout->downloaded survivor_socket_open=true tracked=0');
        });
    } finally { zlib.unzip = originalUnzip; }
    await scenario('store', (req, res) => {
        res.writeHead(200, {'Content-Type': 'text/html'}); res.flushHeaders();
    }, async crawler => {
        const update = crawler.queue.update.bind(crawler.queue);
        const metadataHeld = deferred(), headersAgain = deferred(); let releaseMetadata;
        crawler.queue.update = (id, changes, callback) => {
            if (changes.stateData && changes.stateData.headers) {
                releaseMetadata = () => update(id, changes, callback); metadataHeld.resolve();
            } else update(id, changes, callback);
        };
        crawler.on('fetchheaders', () => headersAgain.resolve());
        crawler.start(); await metadataHeld.promise;
        const request = crawler._openRequests[0]; const timedout = once(crawler, 'fetchtimeout');
        request.emit('timeout'); const [item] = await timedout; assert.equal(item.status, 'timeout');
        releaseMetadata(); await headersAgain.promise; await turn();
        assert.equal(item.fetched, true); assert.equal(item.status, 'headers');
        console.log('LATE_STORE_CORRUPTS fetched=true timeout->headers after_delayed_metadata=true');
    });
    console.log('LIFECYCLE_BASELINE_OK cases=7');
})().catch(error => { console.error(error); process.exitCode = 1; });
