'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const http = require('node:http');
const os = require('node:os');
const path = require('node:path');
const { test } = require('node:test');

const Crawler = require('../lib');

function tick(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms || 50));
}

// Starts a local HTTP server. Every request is recorded with the exact method,
// URL, bytes received and headers, and `handler` decides what to send back.
function startServer(handler) {
    const received = [];

    const server = http.createServer((req, res) => {
        const chunks = [];
        req.on('data', (chunk) => chunks.push(chunk));
        req.on('end', () => {
            const body = Buffer.concat(chunks);
            const record = {
                method: req.method,
                url: req.url,
                body,
                headers: req.headers
            };
            received.push(record);

            const descriptor = handler(record);
            const payload = Buffer.isBuffer(descriptor.body) ?
                descriptor.body :
                Buffer.from(descriptor.body || '', 'utf8');

            const headers = Object.assign({ 'Content-Type': 'text/html' }, descriptor.headers);
            res.writeHead(descriptor.status || 200, headers);
            res.end(payload);
        });
        req.on('error', (error) => res.destroy(error));
    });

    return new Promise((resolve, reject) => {
        server.on('error', reject);
        server.listen(0, '127.0.0.1', () => {
            resolve({
                server,
                received,
                base: 'http://127.0.0.1:' + server.address().port,
                close: () => new Promise((resolve) => server.close(resolve))
            });
        });
    });
}

function makeCrawler(base) {
    const crawler = new Crawler(base + '/');
    crawler.interval = 1;
    crawler.respectRobotsTxt = false;
    crawler.acceptCookies = false;
    return crawler;
}

function runCrawl(crawler) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            crawler.stop(true);
            reject(new Error('crawl stalled'));
        }, 8000);

        crawler.on('fetchclienterror', (queueItem, error) => {
            clearTimeout(timer);
            crawler.stop(true);
            reject(error);
        });

        crawler.on('complete', () => {
            clearTimeout(timer);
            resolve();
        });

        crawler.start();
    });
}

function where(received, method, url) {
    return received.filter((record) => record.method === method && record.url === url);
}

test('Request identity', async () => {
    const multibyte = 'こんにちは'; // 5 characters, 15 UTF-8 bytes
    const zeroBuffer = Buffer.from([0x00, 0xff, 0x10, 0x00, 0x7f]);

    const fixture = await startServer((record) => {
        if (record.method === 'GET' && record.url === '/') {
            return { body: '<a href="/linked">linked</a>' };
        }
        if (record.method === 'POST' && record.url === '/catalog') {
            return { body: '<a href="/catalog-child">child</a>' };
        }
        return { body: 'ok' };
    });

    try {
        const crawler = makeCrawler(fixture.base);
        // A custom Content-Type must not win over a POST request's own type.
        crawler.customHeaders = { 'content-type': 'text/plain', 'x-test': 'yes' };

        const duplicates = [];
        crawler.on('queueduplicate', (item) => duplicates.push(item.requestKey));

        // Synchronous validation happens before any admission check.
        assert.throws(
            () => crawler.queueRequest({ url: fixture.base + '/x', method: 'PUT' }),
            TypeError
        );
        assert.throws(
            () => crawler.queueRequest({ url: fixture.base + '/x', method: 'GET', body: 'nope' }),
            TypeError
        );
        assert.throws(
            () => crawler.queueRequest({ url: fixture.base + '/x', method: 'POST', body: 42 }),
            TypeError
        );
        assert.throws(() => crawler.queueRequest(null), TypeError);

        const enqueued = crawler.queueRequest({ url: fixture.base + '/lower', method: 'post', body: 'x' });
        assert.equal(typeof enqueued, 'boolean');
        assert.equal(typeof enqueued.then, 'undefined');

        const compact = '{"q":1}';
        const spaced = '{ "q": 1 }';

        // Three distinct POST identities on one URL plus one GET identity.
        crawler.queueRequest({
            url: fixture.base + '/search', method: 'POST', body: compact, contentType: 'application/json'
        });
        crawler.queueRequest({
            url: fixture.base + '/search', method: 'POST', body: compact, contentType: 'application/json'
        });
        crawler.queueRequest({
            url: fixture.base + '/search', method: 'POST', body: spaced, contentType: 'application/json'
        });
        crawler.queueRequest({ url: fixture.base + '/search', method: 'POST' });
        crawler.queueURL(fixture.base + '/search');
        crawler.queueRequest({ url: fixture.base + '/search' });

        // Multibyte text and a Buffer containing zero bytes.
        crawler.queueRequest({
            url: fixture.base + '/echo', method: 'POST', body: multibyte,
            contentType: 'text/plain; charset=utf-8'
        });
        crawler.queueRequest({
            url: fixture.base + '/echo', method: 'POST', body: zeroBuffer,
            contentType: 'application/octet-stream'
        });

        // Mutation after enqueue must not change the queued bytes.
        const mutableBuffer = Buffer.from('original-body');
        crawler.queueRequest({ url: fixture.base + '/mut', method: 'POST', body: mutableBuffer });
        const mutableObject = { url: fixture.base + '/mut2', method: 'POST', body: 'obj-original' };
        crawler.queueRequest(mutableObject);
        mutableBuffer.fill(0x58);
        mutableObject.body = 'changed';

        // A POST response still feeds normal link discovery.
        crawler.queueRequest({
            url: fixture.base + '/catalog', method: 'POST', body: 'page=1',
            contentType: 'application/x-www-form-urlencoded'
        });

        await tick();
        await runCrawl(crawler);

        // One URL, four request identities; the two repeated calls were dupes.
        const searchItems = crawler.queue.filter((item) => item.url === fixture.base + '/search');
        assert.equal(searchItems.length, 4);
        const searchKeys = new Set(searchItems.map((item) => item.requestKey));
        assert.equal(searchKeys.size, 4);
        assert.equal(duplicates.length, 2);

        // The wire saw exactly three POSTs and one GET for /search.
        const postSearches = where(fixture.received, 'POST', '/search');
        const getSearches = where(fixture.received, 'GET', '/search');
        assert.equal(postSearches.length, 3);
        assert.equal(getSearches.length, 1);

        const postBodies = postSearches.map((record) => record.body.toString('utf8')).sort();
        assert.deepEqual(postBodies, ['', spaced, compact]);

        // POST keeps its own Content-Type; Content-Length is measured in bytes.
        const jsonPost = postSearches.find((record) => record.body.toString('utf8') === compact);
        assert.equal(jsonPost.headers['content-type'], 'application/json');
        assert.equal(jsonPost.headers['content-length'], String(Buffer.byteLength(compact)));
        assert.equal(jsonPost.headers['x-test'], 'yes');

        const emptyPost = postSearches.find((record) => record.body.length === 0);
        assert.equal(emptyPost.headers['content-type'], 'application/octet-stream');
        assert.equal(emptyPost.headers['content-length'], '0');

        // A GET has no body headers of its own, but customHeaders still apply
        // to it - only a POST's own Content-Type is protected.
        assert.equal(getSearches[0].body.length, 0);
        assert.equal(getSearches[0].headers['content-type'], 'text/plain');

        // Multibyte text goes out as UTF-8 bytes with a byte-accurate length.
        const multibyteRecord = where(fixture.received, 'POST', '/echo')
            .find((record) => record.headers['content-type'] === 'text/plain; charset=utf-8');
        assert.equal(multibyteRecord.body.toString('utf8'), multibyte);
        assert.equal(multibyteRecord.body.length, Buffer.byteLength(multibyte));
        assert.equal(multibyteRecord.headers['content-length'], '15');

        // Zero bytes inside a Buffer survive unchanged.
        const binaryRecord = where(fixture.received, 'POST', '/echo')
            .find((record) => record.headers['content-type'] === 'application/octet-stream');
        assert.deepEqual(binaryRecord.body, zeroBuffer);

        // Defensive copies isolate queued requests from later input mutation.
        assert.equal(where(fixture.received, 'POST', '/mut')[0].body.toString('utf8'), 'original-body');
        assert.equal(where(fixture.received, 'POST', '/mut2')[0].body.toString('utf8'), 'obj-original');

        // Discovered links and the lower-cased method both go out as GET/POST.
        assert.equal(where(fixture.received, 'GET', '/linked').length, 1);
        assert.equal(where(fixture.received, 'GET', '/catalog-child').length, 1);
        assert.equal(where(fixture.received, 'GET', '/catalog-child')[0].body.length, 0);
        assert.equal(where(fixture.received, 'POST', '/lower').length, 1);
        assert.equal(where(fixture.received, 'POST', '/catalog')[0].headers['content-type'],
            'application/x-www-form-urlencoded');
    } finally {
        await fixture.close();
    }
});

test('Redirect method', async () => {
    const fixture = await startServer((record) => {
        const route = record.method + ' ' + record.url;

        if (route === 'POST /r301') {
            return { status: 301, headers: { Location: '/land301' }, body: '' };
        }
        if (route === 'POST /r302') {
            return { status: 302, headers: { Location: '/land302' }, body: '' };
        }
        if (route === 'POST /r303') {
            return { status: 303, headers: { Location: '/result' }, body: '' };
        }
        if (route === 'POST /r307') {
            return { status: 307, headers: { Location: '/same' }, body: '' };
        }
        if (route === 'POST /r308') {
            return { status: 308, headers: { Location: '/keep8' }, body: '' };
        }
        return { body: 'ok' };
    });

    try {
        const crawler = makeCrawler(fixture.base);
        const redirects = [];
        const duplicates = [];
        crawler.on('fetchredirect', (queueItem, redirectQueueItem) => {
            redirects.push({
                source: queueItem.url,
                method: redirectQueueItem.method,
                body: Buffer.isBuffer(redirectQueueItem.body) ? redirectQueueItem.body : Buffer.alloc(0),
                contentType: redirectQueueItem.contentType
            });
        });
        crawler.on('queueduplicate', (item) => duplicates.push(item));

        // Fetch the 303 target first, so the redirected GET must be deduped.
        crawler.queueURL(fixture.base + '/result');

        crawler.queueRequest({ url: fixture.base + '/r301', method: 'POST', body: 'go-301' });
        crawler.queueRequest({ url: fixture.base + '/r302', method: 'POST', body: 'go-302' });
        crawler.queueRequest({
            url: fixture.base + '/r303', method: 'POST', body: 'go-303', contentType: 'application/json'
        });
        crawler.queueRequest({
            url: fixture.base + '/r307', method: 'POST', body: 'redirect-same', contentType: 'application/json'
        });
        crawler.queueRequest({
            url: fixture.base + '/r308', method: 'POST', body: 'redirect-8', contentType: 'application/json'
        });

        // Same URLs as the 307/308 targets but different bodies and types:
        // URL-only dedupe must not swallow these distinct POST identities.
        crawler.queueRequest({
            url: fixture.base + '/same', method: 'POST', body: 'direct-same', contentType: 'text/plain'
        });
        crawler.queueRequest({
            url: fixture.base + '/keep8', method: 'POST', body: 'direct-8', contentType: 'text/plain'
        });

        await tick();
        await runCrawl(crawler);

        // 301/302/303 turn POST into an empty GET and strip body headers.
        for (const land of ['/land301', '/land302']) {
            const landRecords = where(fixture.received, 'GET', land);
            assert.equal(landRecords.length, 1);
            assert.equal(landRecords[0].body.length, 0);
            assert.equal(landRecords[0].headers['content-type'], undefined);
        }

        // The 303 points at an already-fetched GET, so it is not refetched.
        assert.equal(where(fixture.received, 'GET', '/result').length, 1);
        assert.equal(duplicates.length, 1);
        assert.equal(duplicates[0].url, fixture.base + '/result');

        // 307/308 preserve method, exact bytes and content type.
        const sameRecords = where(fixture.received, 'POST', '/same');
        assert.equal(sameRecords.length, 2);
        assert.deepEqual(
            sameRecords.map((record) => record.body.toString('utf8')).sort(),
            ['direct-same', 'redirect-same']
        );
        assert.deepEqual(
            sameRecords.map((record) => record.headers['content-type']).sort(),
            ['application/json', 'text/plain']
        );

        const keep8Records = where(fixture.received, 'POST', '/keep8');
        assert.equal(keep8Records.length, 2);
        assert.deepEqual(
            keep8Records.map((record) => record.body.toString('utf8')).sort(),
            ['direct-8', 'redirect-8']
        );

        const bySource = {};
        for (const redirect of redirects) {
            bySource[redirect.source] = redirect;
        }

        for (const suffix of ['/r301', '/r302', '/r303']) {
            const target = bySource[fixture.base + suffix];
            assert.equal(target.method, 'GET');
            assert.equal(target.body.length, 0);
            assert.equal(target.contentType, undefined);
        }

        for (const suffix of ['/r307', '/r308']) {
            const target = bySource[fixture.base + suffix];
            assert.equal(target.method, 'POST');
            assert.equal(target.contentType, 'application/json');
        }
        assert.equal(bySource[fixture.base + '/r307'].body.toString('utf8'), 'redirect-same');
        assert.equal(bySource[fixture.base + '/r308'].body.toString('utf8'), 'redirect-8');

        // The redirect sources themselves went out as POSTs with their bodies.
        assert.equal(where(fixture.received, 'POST', '/r303')[0].body.toString('utf8'), 'go-303');
    } finally {
        await fixture.close();
    }
});

test('Persisted bodies', async () => {
    const multibyte = 'テスト'; // 3 characters, 9 UTF-8 bytes
    const zeroBuffer = Buffer.from([0x00, 0x01, 0x00, 0xfe]);

    const fixture = await startServer(() => ({ body: 'ok' }));
    const queueFile = path.join(os.tmpdir(), 'simplecrawler-queue-' + process.pid + '.json');
    const legacyFile = path.join(os.tmpdir(), 'simplecrawler-legacy-' + process.pid + '.json');

    try {
        const crawler = makeCrawler(fixture.base);
        crawler.queueRequest({
            url: fixture.base + '/p1', method: 'POST', body: multibyte,
            contentType: 'text/plain; charset=utf-8'
        });
        crawler.queueRequest({
            url: fixture.base + '/p2', method: 'POST', body: zeroBuffer,
            contentType: 'application/octet-stream'
        });
        crawler.queueURL(fixture.base + '/g');
        await tick();

        await new Promise((resolve, reject) => {
            crawler.queue.freeze(queueFile, (error) => (error ? reject(error) : resolve()));
        });

        // The frozen file keeps binary bodies (as JSON Buffers) and identities.
        const raw = JSON.parse(fs.readFileSync(queueFile, 'utf8'));
        assert.equal(raw.length, 3);
        assert.equal(raw[0].body.type, 'Buffer');
        assert.ok(Array.isArray(raw[0].body.data));
        assert.equal(typeof raw[0].requestKey, 'string');
        assert.equal(raw[1].body.data[0], 0);

        const restored = makeCrawler(fixture.base);
        await new Promise((resolve, reject) => {
            restored.queue.defrost(queueFile, (error) => (error ? reject(error) : resolve()));
        });

        assert.equal(restored.queue.length, 3);
        assert.equal(restored.queue[0].method, 'POST');
        assert.equal(Buffer.isBuffer(restored.queue[0].body), true);
        assert.equal(restored.queue[0].body.toString('utf8'), multibyte);
        assert.equal(restored.queue[0].contentType, 'text/plain; charset=utf-8');
        assert.deepEqual(restored.queue[1].body, zeroBuffer);
        assert.equal(restored.queue[2].method, 'GET');
        assert.equal(restored.queue[2].body.length, 0);

        for (let i = 0; i < 3; i++) {
            assert.equal(restored.queue[i].requestKey, crawler.queue[i].requestKey);
        }

        // Identities participate in dedupe after defrost; another body does not.
        let duplicateCount = 0;
        restored.on('queueduplicate', () => duplicateCount++);
        restored.queueRequest({
            url: fixture.base + '/p1', method: 'POST', body: multibyte,
            contentType: 'text/plain; charset=utf-8'
        });
        restored.queueRequest({
            url: fixture.base + '/p1', method: 'POST', body: 'different',
            contentType: 'text/plain; charset=utf-8'
        });
        await tick();
        assert.equal(duplicateCount, 1);
        assert.equal(restored.queue.length, 4);

        // The defrosted queue re-sends the preserved bytes over the wire.
        await runCrawl(restored);

        const p1 = where(fixture.received, 'POST', '/p1');
        assert.ok(p1.some((record) => record.body.toString('utf8') === multibyte));
        assert.ok(p1.some((record) => record.headers['content-type'] === 'text/plain; charset=utf-8'));

        const p2 = where(fixture.received, 'POST', '/p2');
        assert.equal(p2.length, 1);
        assert.deepEqual(p2[0].body, zeroBuffer);
        assert.equal(p2[0].headers['content-type'], 'application/octet-stream');

        const g = where(fixture.received, 'GET', '/g');
        assert.equal(g.length, 1);
        assert.equal(g[0].body.length, 0);
        assert.equal(g[0].headers['content-type'], undefined);

        // A legacy queue file containing only URLs loads as bodiless GET.
        fs.writeFileSync(legacyFile, JSON.stringify([{
            id: 0,
            url: fixture.base + '/old',
            protocol: 'http',
            host: '127.0.0.1',
            port: fixture.server.address().port,
            path: '/old',
            depth: 1,
            fetched: true,
            status: 'downloaded',
            stateData: {}
        }]));

        const legacy = makeCrawler(fixture.base);
        await new Promise((resolve, reject) => {
            legacy.queue.defrost(legacyFile, (error) => (error ? reject(error) : resolve()));
        });

        assert.equal(legacy.queue[0].method, 'GET');
        assert.equal(Buffer.isBuffer(legacy.queue[0].body), true);
        assert.equal(legacy.queue[0].body.length, 0);
        assert.equal(typeof legacy.queue[0].requestKey, 'string');

        let legacyDuplicates = 0;
        legacy.on('queueduplicate', () => legacyDuplicates++);
        legacy.queueURL(fixture.base + '/old');
        await tick();
        assert.equal(legacyDuplicates, 1);
    } finally {
        await fixture.close();
        for (const file of [queueFile, legacyFile]) {
            try {
                fs.unlinkSync(file);
            } catch (error) {
                // The file may never have been written if the test failed early.
            }
        }
    }
});
