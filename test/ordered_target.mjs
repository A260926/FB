import test from "node:test";
import assert from "node:assert/strict";
import http from "node:http";
import Crawler from "../src/index.ts";

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function waitFor(predicate, { timeout = 5000, interval = 5, message = "waitFor timed out" } = {}) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
        if (predicate()) return;
        await sleep(interval);
    }
    assert.ok(predicate(), message);
}

async function startServer(handler) {
    const server = http.createServer(handler);
    await new Promise((resolve, reject) => {
        server.once("error", reject);
        server.listen(0, "127.0.0.1", resolve);
    });
    return {
        server,
        base: `http://127.0.0.1:${server.address().port}`,
    };
}

function drainTracker(crawler) {
    let count = 0;
    let resolveDrain;
    const firstDrain = new Promise((resolve) => {
        resolveDrain = resolve;
    });
    const onDrain = () => {
        count++;
        resolveDrain();
    };
    crawler.on("drain", onDrain);
    return {
        get count() {
            return count;
        },
        first: firstDrain,
        detach() {
            crawler.off("drain", onDrain);
        },
    };
}

// Generic route bookkeeping: hit order, per-path counts and fully-sent responses.
function bookkeeping(state) {
    return (req, res) => {
        const n = (state.counts[req.url] = (state.counts[req.url] || 0) + 1);
        state.hits.push(req.url);
        res.on("finish", () => state.finished.add(`${req.url}#${n}`));
    };
}

test("ordered dispatch", async () => {
    const state = { hits: [], counts: {}, finished: new Set(), gates: {} };
    const note = bookkeeping(state);
    const { server, base } = await startServer((req, res) => {
        note(req, res);
        if (req.url === "/a") {
            state.gates.a = res; // held open until the test releases it
            return;
        }
        if (req.url === "/db") {
            state.gates.db = res;
            if (state.releaseDb) res.end("DA"); // /dbb may arrive first
            return;
        }
        if (req.url === "/dbb") {
            // Release the held /db response from the server side so default
            // completion-order delivery (/dbb's callback first) can proceed.
            if (state.gates.db) state.gates.db.end("DA");
            else state.releaseDb = true;
            res.end("DB");
            return;
        }
        if (req.url === "/g") {
            state.gates.g = res;
            return;
        }
        res.end(req.url.slice(1).toUpperCase());
    });

    try {
        // --- Cross-limiter gate: B finishes downloading while A is still held open.
        const crawler = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const drain = drainTracker(crawler);
        const log = [];

        crawler.add({
            url: `${base}/a`,
            rateLimiterId: 0,
            callback: (error, response, done) => {
                assert.ifError(error);
                log.push("A enter");
                assert.equal(log.includes("B enter"), false, "B must wait for A's callback");
                assert.equal(response.body, "A");
                done();
                log.push("A return");
            },
        });
        crawler.add({
            url: `${base}/b`,
            rateLimiterId: 1,
            callback: (error, response, done) => {
                assert.ifError(error);
                log.push("B enter");
                assert.ok(log.includes("A return"), "B must not nest inside A's callback body");
                assert.equal(response.body, "B");
                done();
                log.push("B return");
            },
        });

        // Wait until both requests have actually been dispatched to the server
        // and B's complete response has been flushed while A is still gated.
        await waitFor(() => state.hits.includes("/a") && state.hits.includes("/b"));
        await waitFor(() => state.finished.has("/b#1"));
        assert.deepEqual([...new Set(state.hits)].sort(), ["/a", "/b"]);
        assert.equal(state.gates.a.writableEnded, false, "A body still held open");
        // ...yet neither callback has been delivered out of order.
        assert.deepEqual(log, []);

        state.gates.a.end("A");
        await drain.first;
        assert.deepEqual(log, ["A enter", "A return", "B enter", "B return"]);
        assert.equal(drain.count, 1);
        drain.detach();

        // --- Default mode keeps the original completion-order delivery.
        const plain = new Crawler({ maxConnections: 2, retries: 0, jQuery: false, silence: true });
        const plainLog = [];
        const plainDrain = drainTracker(plain);
        plain.add([
            {
                url: `${base}/db`,
                callback: (error, response, done) => {
                    plainLog.push("DA");
                    done();
                },
            },
            {
                url: `${base}/dbb`,
                callback: (error, response, done) => {
                    plainLog.push("DB");
                    done();
                },
            },
        ]);
        await plainDrain.first;
        assert.deepEqual(plainLog, ["DB", "DA"]);
        plainDrain.detach();

        // --- Priority, not add order, decides the start sequence.
        const priorityCrawler = new Crawler({
            maxConnections: 1,
            retries: 0,
            jQuery: false,
            silence: true,
            priorityLevels: 3,
            orderedCallbacks: true,
        });
        const pDrain = drainTracker(priorityCrawler);
        const pLog = [];
        priorityCrawler.add({
            url: `${base}/g`,
            callback: (error, response, done) => {
                pLog.push("G");
                done();
            },
        });
        await waitFor(() => Boolean(state.gates.g), { message: "gate task never occupied the single slot" });
        // Added in order X then Y, but Y has the higher priority (lower value wins).
        priorityCrawler.add([
            {
                url: `${base}/x`,
                priority: 2,
                callback: (error, response, done) => {
                    pLog.push("X");
                    done();
                },
            },
            {
                url: `${base}/y`,
                priority: 0,
                callback: (error, response, done) => {
                    pLog.push("Y");
                    done();
                },
            },
        ]);
        state.gates.g.end("G");
        await pDrain.first;
        const tailHits = state.hits.slice(state.hits.indexOf("/g"));
        assert.deepEqual(tailHits, ["/g", "/y", "/x"], "limiter must start Y before X by priority");
        assert.deepEqual(pLog, ["G", "Y", "X"], "callbacks follow limiter start order, not add order");
        pDrain.detach();

        // --- skipDuplicates rejects a candidate before dispatch, so it takes no slot.
        const skipCrawler = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            skipDuplicates: true,
            orderedCallbacks: true,
        });
        const sDrain = drainTracker(skipCrawler);
        const sLog = [];
        const dupCallback = (error, response, done) => {
            sLog.push(response.body);
            done();
        };
        skipCrawler.add({ url: `${base}/dup`, callback: dupCallback });
        await sDrain.first; // seenreq has recorded /dup once it was dispatched
        const secondDrainDone = new Promise((resolve) => {
            skipCrawler.on("drain", resolve);
        });
        skipCrawler.add([
            { url: `${base}/dup`, callback: dupCallback }, // duplicate: never dispatched
            { url: `${base}/uniq`, callback: dupCallback },
        ]);
        await secondDrainDone;
        assert.equal(state.counts["/dup"], 1, "duplicate candidate must not be dispatched");
        assert.equal(state.counts["/uniq"], 1);
        assert.deepEqual(sLog, ["DUP", "UNIQ"], "no hole is left by the rejected duplicate");
        sDrain.detach();
    } finally {
        Object.values(state.gates).forEach((res) => {
            if (!res.writableEnded) res.destroy();
        });
        await new Promise((resolve) => server.close(resolve));
    }
});

test("ordered failure", async () => {
    const state = { hits: [], counts: {}, finished: new Set() };
    const note = bookkeeping(state);
    const { server, base } = await startServer((req, res) => {
        note(req, res);
        if (req.url.startsWith("/fail")) {
            res.statusCode = 500;
            res.end("failure");
            return;
        }
        res.end("ok");
    });

    try {
        const crawler = new Crawler({
            maxConnections: 2,
            retries: 1,
            retryInterval: 30,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const drain = drainTracker(crawler);
        const log = [];
        const dones = {};
        let failCallbacks = 0;

        crawler.add({
            url: `${base}/fail`,
            callback: (error, response, done) => {
                failCallbacks++;
                dones.fail = done;
                log.push("F");
                assert.ok(error, "final failure must deliver the original error");
                assert.equal(response.options.url, `${base}/fail`);
                done();
            },
        });
        crawler.add({
            url: `${base}/ok`,
            callback: (error, response, done) => {
                dones.ok = done;
                log.push("S");
                assert.ifError(error);
                assert.equal(response.body, "ok");
                // The successful result waits behind the failed entry.
                assert.deepEqual(log.slice(0, 1), ["F"]);
                done();
            },
        });

        await drain.first;
        assert.equal(state.counts["/fail"], 2, "the request retries but keeps its single sequence slot");
        assert.equal(failCallbacks, 1, "mid-flight retries are never delivered");
        assert.deepEqual(log, ["F", "S"], "the exhausted retry fills its hole before S is delivered");
        assert.notEqual(dones.fail, dones.ok, "each callback receives its own done");
        assert.equal(drain.count, 1);
        drain.detach();

        // A request with retries disabled still fails in order and unblocks the next one.
        const second = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const secondDrain = drainTracker(second);
        const secondLog = [];
        second.add([
            {
                url: `${base}/fail0`,
                callback: (error, response, done) => {
                    secondLog.push("G");
                    assert.ok(error);
                    done();
                },
            },
            {
                url: `${base}/ok2`,
                callback: (error, response, done) => {
                    secondLog.push("T");
                    assert.ifError(error);
                    done();
                },
            },
        ]);
        await secondDrain.first;
        assert.equal(state.counts["/fail0"], 1);
        assert.deepEqual(secondLog, ["G", "T"]);
        assert.equal(secondDrain.count, 1);
        secondDrain.detach();
    } finally {
        await new Promise((resolve) => server.close(resolve));
    }
});

test("ordered lifecycle", async () => {
    const state = { hits: [], counts: {}, finished: new Set() };
    const note = bookkeeping(state);
    const { server, base } = await startServer((req, res) => {
        note(req, res);
        res.end(req.url.slice(1).toUpperCase());
    });
    let unhandled = 0;
    const onUnhandled = () => {
        unhandled++;
    };
    process.on("unhandledRejection", onUnhandled);

    try {
        // --- Delayed, duplicated done: release once, no nested delivery.
        const delayed = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const dDrain = drainTracker(delayed);
        const dLog = [];
        delayed.add([
            {
                url: `${base}/p1`,
                callback: (error, response, done) => {
                    dLog.push("P1 enter");
                    setTimeout(() => {
                        dLog.push("P1 done-call");
                        done();
                        done(); // idempotent: must not release anyone else's slot
                    }, 40);
                    dLog.push("P1 return");
                },
            },
            {
                url: `${base}/p2`,
                callback: (error, response, done) => {
                    dLog.push("P2 enter");
                    assert.ok(dLog.includes("P1 done-call"), "P2 waits for P1's delayed done");
                    done();
                    dLog.push("P2 return");
                },
            },
        ]);
        await dDrain.first;
        assert.deepEqual(dLog, ["P1 enter", "P1 return", "P1 done-call", "P2 enter", "P2 return"]);
        assert.equal(dDrain.count, 1);
        dDrain.detach();

        // --- Synchronous add() inside a callback cannot reorder or skip entries.
        const syncAdd = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const aDrain = drainTracker(syncAdd);
        const aLog = [];
        syncAdd.add([
            {
                url: `${base}/q1`,
                callback: (error, response, done) => {
                    aLog.push("Q1 enter");
                    syncAdd.add({
                        url: `${base}/q3`,
                        callback: (error2, response2, done2) => {
                            aLog.push("Q3 enter");
                            done2();
                        },
                    });
                    done();
                    aLog.push("Q1 return");
                },
            },
            {
                url: `${base}/q2`,
                callback: (error, response, done) => {
                    aLog.push("Q2 enter");
                    assert.ok(aLog.includes("Q1 return"), "Q2 must not nest inside Q1's body");
                    done();
                    aLog.push("Q2 return");
                },
            },
        ]);
        await aDrain.first;
        assert.deepEqual(aLog, ["Q1 enter", "Q1 return", "Q2 enter", "Q2 return", "Q3 enter"]);
        assert.equal(state.counts["/q3"], 1);
        assert.equal(aDrain.count, 1);
        aDrain.detach();

        // --- A held done keeps occupying the limiter slot: buffered responses
        // stay bounded by maxConnections instead of dispatching everything.
        const pressure = new Crawler({
            maxConnections: 1,
            retries: 0,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const pressureDrain = drainTracker(pressure);
        const pressureLog = [];
        let releaseFirst;
        pressure.add([
            {
                url: `${base}/r1`,
                callback: (error, response, done) => {
                    pressureLog.push("R1");
                    releaseFirst = done; // hold the only slot
                },
            },
            {
                url: `${base}/r2`,
                callback: (error, response, done) => {
                    pressureLog.push("R2");
                    done();
                },
            },
            {
                url: `${base}/r3`,
                callback: (error, response, done) => {
                    pressureLog.push("R3");
                    done();
                },
            },
        ]);
        await waitFor(() => pressureLog.length === 1, { message: "first callback never ran" });
        await sleep(20);
        assert.equal(state.counts["/r1"], 1, "the held slot must block further dispatches");
        assert.equal(state.counts["/r2"], undefined, "no second request may pass the connection limit");
        assert.equal(state.counts["/r3"], undefined);
        assert.deepEqual(pressureLog, ["R1"]);
        releaseFirst();
        await pressureDrain.first;
        assert.equal(state.counts["/r2"], 1);
        assert.equal(state.counts["/r3"], 1);
        assert.deepEqual(pressureLog, ["R1", "R2", "R3"]);
        assert.equal(pressureDrain.count, 1);
        pressureDrain.detach();

        // --- html entries, thrown/rejected callbacks, implicit done and drain waiting.
        const life = new Crawler({
            maxConnections: 2,
            retries: 0,
            jQuery: false,
            silence: true,
            orderedCallbacks: true,
        });
        const lifeDrain = drainTracker(life);
        const lifeLog = [];
        const callbackErrors = [];
        life.on("callbackError", (error) => callbackErrors.push(error.message));
        let savedDone;

        life.add({
            html: "<html><body><p>hello</p></body></html>",
            jQuery: true,
            callback: (error, response, done) => {
                lifeLog.push("H");
                assert.ifError(error);
                assert.equal(typeof response.$, "function", "html responses still get the DOM");
                assert.ok(response.body.includes("hello"));
                done();
            },
        });
        life.add({
            url: `${base}/t1`,
            callback: (error, response, done) => {
                lifeLog.push("T1");
                done();
                throw new Error("sync boom"); // done first, then throw: still one release only
            },
        });
        life.add({
            url: `${base}/t2`,
            callback: () => Promise.reject(new Error("async boom")), // no done: rejection ends it
        });
        life.add({
            url: `${base}/t3`,
            callback: (error, response, done) => {
                lifeLog.push("T3");
                savedDone = done;
                return Promise.resolve("resolved promises are not an implicit done");
            },
        });
        life.add({
            url: `${base}/t4`,
            callback: (error, response, done) => {
                lifeLog.push("T4");
                done();
            },
        });

        await waitFor(
            () => lifeLog.length === 3 && callbackErrors.length === 2,
            { message: "lifecycle callbacks did not settle at the T3 barrier" }
        );
        assert.deepEqual(lifeLog, ["H", "T1", "T3"], "T4 stays blocked while T3 waits for its done");
        assert.deepEqual(callbackErrors, ["sync boom", "async boom"]);
        assert.equal(lifeDrain.count, 0, "drain waits for the undelivered callback");
        assert.equal(state.counts["/t2"], 1, "a rejected callback never retries the request");

        savedDone();
        await lifeDrain.first;
        assert.deepEqual(lifeLog, ["H", "T1", "T3", "T4"]);
        assert.equal(lifeDrain.count, 1);
        assert.equal(unhandled, 0, "rejected callback promises must not become unhandled rejections");
        lifeDrain.detach();
    } finally {
        process.off("unhandledRejection", onUnhandled);
        await new Promise((resolve) => server.close(resolve));
    }
});
