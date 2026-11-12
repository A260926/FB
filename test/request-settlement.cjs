"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");
const http = require("node:http");
const zlib = require("node:zlib");
const Crawler = require("../");

const turn = () => new Promise(resolve => setImmediate(resolve));

function deferred() {
    let resolve;
    const promise = new Promise(r => {
        resolve = r;
    });
    return { promise, resolve };
}

function onceEvent(emitter, name) {
    return new Promise(resolve => {
        emitter.once(name, (...args) => resolve(args));
    });
}

async function startServer(handler) {
    const sockets = new Set();
    const server = http.createServer(handler);

    server.on("connection", socket => {
        sockets.add(socket);
        socket.on("error", () => {});
        socket.on("close", () => sockets.delete(socket));
    });

    await new Promise((resolve, reject) => {
        server.once("error", reject);
        server.listen(0, "127.0.0.1", () => {
            server.removeListener("error", reject);
            resolve();
        });
    });

    return {
        base: `http://127.0.0.1:${server.address().port}`,
        stop: async () => {
            for (const socket of sockets) {
                socket.destroy();
            }
            await new Promise(resolve => server.close(resolve));
        }
    };
}

async function withServer(handler, run) {
    const server = await startServer(handler);
    try {
        return await run(server.base);
    } finally {
        await server.stop();
    }
}

function makeCrawler(base, path, settings) {
    const crawler = new Crawler(base + path);
    crawler.interval = 1;
    crawler.timeout = 60000;
    crawler.respectRobotsTxt = false;
    crawler.discoverResources = false;
    Object.assign(crawler, settings || {});
    return crawler;
}

function disposeCrawler(crawler) {
    crawler.stop(true);
    crawler.httpAgent.destroy();
    crawler.httpsAgent.destroy();
}

function recordEvents(crawler, names) {
    const records = [];

    names.forEach(name => {
        crawler.on(name, (item, value, extra) => {
            records.push({
                name,
                item,
                value,
                extra,
                path: item && item.path
            });
        });
    });

    return records;
}

function eventsFor(records, path) {
    return records.filter(record => record.path === path).map(record => record.name);
}

function queueItem(crawler, path) {
    return crawler.queue.find(item => item.path === path);
}

test("Request settlement", async t => {
    await t.test("delivers a complete body and discovers links exactly once", async () => {
        const pageBody = "<html><body><a href=\"/linked\">next</a></body></html>";
        const linkedBody = "linked body";

        await withServer((req, res) => {
            res.on("error", () => {});

            if (req.url === "/page") {
                res.writeHead(200, {
                    "Content-Type": "text/html",
                    "Content-Length": String(Buffer.byteLength(pageBody))
                });
                res.end(pageBody);
            } else if (req.url === "/linked") {
                res.writeHead(200, {
                    "Content-Type": "text/html",
                    "Content-Length": String(Buffer.byteLength(linkedBody))
                });
                res.end(linkedBody);
            } else {
                res.writeHead(404, {"Content-Type": "text/html"});
                res.end("missing");
            }
        }, async base => {
            const crawler = makeCrawler(base, "/page");
            // The default discoverResources implementation is the prototype
            // function (truthy). Replace the instance boolean so link
            // discovery is enabled without clobbering the implementation.
            delete crawler.discoverResources;
            const records = recordEvents(crawler, ["fetchcomplete", "fetchclienterror", "fetchdataerror"]);
            const delivered = [];
            const closed = [];
            let discoveryCalls = 0;
            const originalQueueLinkedItems = crawler.queueLinkedItems.bind(crawler);
            const complete = onceEvent(crawler, "complete");

            crawler.queueLinkedItems = function (body, item) {
                discoveryCalls++;
                return originalQueueLinkedItems(body, item);
            };

            crawler.on("fetchcomplete", (item, body, response) => {
                assert.ok(response instanceof http.IncomingMessage);
                delivered.push({
                    path: item.path,
                    body: body.toString()
                });
            });

            crawler.on("fetchheaders", (item, response) => {
                response.on("close", () => closed.push(item.path));
            });

            try {
                crawler.start();
                await complete;

                assert.equal(delivered.length, 2);
                assert.equal(eventsFor(records, "/page").filter(name => name === "fetchcomplete").length, 1);
                assert.equal(eventsFor(records, "/linked").filter(name => name === "fetchcomplete").length, 1);
                assert.equal(delivered.find(entry => entry.path === "/page").body, pageBody);
                assert.equal(delivered.find(entry => entry.path === "/linked").body, linkedBody);
                assert.ok(discoveryCalls >= 1);
                assert.equal(crawler.queue.length, 2);

                const page = queueItem(crawler, "/page");
                const linked = queueItem(crawler, "/linked");

                assert.equal(page.status, "downloaded");
                assert.equal(page.fetched, true);
                assert.equal(linked.status, "downloaded");
                assert.equal(linked.fetched, true);

                // A normal end followed by close is not a failure.
                assert.equal(records.some(record => record.name === "fetchclienterror"), false);
                assert.equal(records.some(record => record.name === "fetchdataerror"), false);
                assert.ok(closed.includes("/page"));
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("settles a known content-length overage as failed, never delivering a body", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Length": "100"
            });
            res.end("x".repeat(100));
        }, async base => {
            const crawler = makeCrawler(base, "/oversize", { maxResourceSize: 8 });
            const records = recordEvents(crawler, ["fetchdataerror", "fetchcomplete", "fetchclienterror"]);
            const delivered = [];
            let cached = 0;
            let discovered = 0;
            const complete = onceEvent(crawler, "complete");

            crawler.cache = {
                setCacheData() {
                    cached++;
                }
            };
            crawler.queueLinkedItems = () => {
                discovered++;
            };
            crawler.on("fetchcomplete", (item, body) => delivered.push(body.length));

            try {
                crawler.start();
                await complete;

                const errors = records.filter(record => record.name === "fetchdataerror");

                assert.equal(errors.length, 1);
                assert.ok(errors[0].value instanceof http.IncomingMessage);
                assert.equal(delivered.length, 0);
                assert.equal(cached, 0);
                assert.equal(discovered, 0);

                const item = queueItem(crawler, "/oversize");

                assert.equal(item.fetched, true);
                assert.equal(item.status, "failed");
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(records.some(record => record.name === "fetchclienterror"), false);
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("settles a chunked overage as failed without publishing the partial body", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Transfer-Encoding": "chunked"
            });
            res.write("x".repeat(32));
            res.end("y".repeat(32));
        }, async base => {
            const crawler = makeCrawler(base, "/chunked-oversize", { maxResourceSize: 8 });
            const records = recordEvents(crawler, ["fetchdataerror", "fetchcomplete", "fetchclienterror"]);
            const delivered = [];
            let cached = 0;
            let discovered = 0;
            const complete = onceEvent(crawler, "complete");

            crawler.cache = {
                setCacheData() {
                    cached++;
                }
            };
            crawler.queueLinkedItems = () => {
                discovered++;
            };
            crawler.on("fetchcomplete", (item, body) => delivered.push(body.length));

            try {
                crawler.start();
                await complete;

                const errors = records.filter(record => record.name === "fetchdataerror");

                assert.equal(errors.length, 1);
                assert.ok(errors[0].value instanceof http.IncomingMessage);
                assert.equal(delivered.length, 0);
                assert.equal(cached, 0);
                assert.equal(discovered, 0);

                const item = queueItem(crawler, "/chunked-oversize");

                assert.equal(item.fetched, true);
                assert.equal(item.status, "failed");
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("reports a real local HTTP truncation through fetchclienterror, preserving the error object", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            req.socket.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Length": "100"
            });
            res.write("partial", () => {
                res.socket.destroy();
            });
        }, async base => {
            const crawler = makeCrawler(base, "/truncate");
            const records = recordEvents(crawler, ["fetchclienterror", "fetchcomplete", "fetchdataerror"]);
            const capturedErrors = [];
            const delivered = [];
            let cached = 0;
            let discovered = 0;
            const complete = onceEvent(crawler, "complete");
            const clientError = onceEvent(crawler, "fetchclienterror");

            crawler.cache = {
                setCacheData() {
                    cached++;
                }
            };
            crawler.queueLinkedItems = () => {
                discovered++;
            };
            crawler.on("fetchheaders", (item, response) => {
                // Registered in front of the crawler's own listener so the
                // original error object can be compared with the emitted one.
                response.prependListener("error", error => capturedErrors.push(error));
                if (response.req) {
                    response.req.prependListener("error", error => capturedErrors.push(error));
                }
            });
            crawler.on("fetchcomplete", (item, body) => delivered.push(body.length));

            try {
                crawler.start();
                const [item, error] = await clientError;

                assert.ok(error instanceof Error);
                assert.ok(capturedErrors.length > 0);
                assert.ok(capturedErrors.indexOf(error) !== -1);
                assert.equal(item.fetched, true);
                assert.equal(item.status, "failed");
                assert.equal(delivered.length, 0);
                assert.equal(cached, 0);
                assert.equal(discovered, 0);
                assert.equal(records.filter(record => record.name === "fetchclienterror").length, 1);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(crawler._openRequests.length, 0);

                await complete;
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("synthesises a discernible error for an early close with no error object", async () => {
        const requestHeld = deferred();

        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {"Content-Type": "text/html"});
            res.flushHeaders();
            requestHeld.resolve(res);
        }, async base => {
            const crawler = makeCrawler(base, "/manual-abort");
            const records = recordEvents(crawler, ["fetchclienterror", "fetchcomplete"]);
            const delivered = [];
            const complete = onceEvent(crawler, "complete");
            const clientError = onceEvent(crawler, "fetchclienterror");

            crawler.on("fetchcomplete", (item, body) => delivered.push(body.length));

            try {
                crawler.start();
                const headers = onceEvent(crawler, "fetchheaders");
                const [, response] = await headers;
                const serverResponse = await requestHeld.promise;

                // Simulate a transport boundary that closes without an error
                // object. The crawler must still produce a distinguishable
                // Error rather than publishing an empty/partial body.
                response.emit("aborted");
                response.emit("close");

                const [item, error] = await clientError;

                assert.ok(error instanceof Error);
                assert.equal(error.code, "PREMATURE_CLOSE");
                assert.equal(item.fetched, true);
                assert.equal(item.status, "failed");
                assert.equal(delivered.length, 0);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(crawler._openRequests.length, 0);

                serverResponse.destroy();
                await complete;
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("catches a transport interruption while an async download condition is pending", async () => {
        const conditionHeld = deferred();
        const responseReady = deferred();

        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {"Content-Type": "text/html"});
            res.flushHeaders();
            responseReady.resolve(res);
        }, async base => {
            const crawler = makeCrawler(base, "/interrupt");
            const records = recordEvents(crawler, ["fetchclienterror", "downloadprevented", "downloadconditionerror", "fetchcomplete"]);
            let releaseCondition;
            const complete = onceEvent(crawler, "complete");
            const clientError = onceEvent(crawler, "fetchclienterror");

            crawler.addDownloadCondition((item, response, callback) => {
                releaseCondition = callback;
                conditionHeld.resolve();
            });

            try {
                crawler.start();
                await conditionHeld.promise;
                const serverResponse = await responseReady.promise;

                // The condition has not called back yet; the transport error
                // must still be caught.
                serverResponse.destroy();

                const [item, error] = await clientError;

                assert.ok(error instanceof Error);
                assert.equal(item.fetched, true);
                assert.equal(item.status, "failed");
                assert.equal(records.some(record => record.name === "downloadprevented"), false);
                assert.equal(records.some(record => record.name === "downloadconditionerror"), false);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);

                releaseCondition(null, false);
                await turn();

                assert.equal(records.some(record => record.name === "downloadprevented"), false);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(crawler._openRequests.length, 0);

                await complete;
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("keeps an unrelated in-flight request when a timeout races a late condition", async () => {
        const conditionHeld = deferred();
        const survivorHeadersSeen = deferred();
        let releaseCondition;
        const survivorHeaders = deferred();
        let lateResponse;

        await withServer((req, res) => {
            res.on("error", () => {});
            if (req.url === "/late") {
                res.writeHead(200, {"Content-Type": "text/html"});
                res.flushHeaders();
                lateResponse = res;
            } else if (req.url === "/survivor") {
                res.writeHead(200, {"Content-Type": "text/html"});
                res.flushHeaders();
                survivorHeaders.resolve(res);
            }
        }, async base => {
            const crawler = makeCrawler(base, "/late", { maxConcurrency: 2 });
            const records = recordEvents(crawler, ["fetchtimeout", "fetchcomplete", "downloadprevented", "fetchdataerror", "fetchclienterror"]);
            const completions = [];
            const started = [];
            const bothStarted = deferred();
            let survivorRequest;

            crawler.queueURL("/survivor");
            crawler.addDownloadCondition((item, response, callback) => {
                if (item.path === "/late") {
                    releaseCondition = callback;
                    conditionHeld.resolve();
                } else {
                    callback(null, true);
                }
            });

            crawler.on("fetchstart", (item, options) => {
                started.push(item.path);
                if (started.length === 2) {
                    bothStarted.resolve();
                }
            });
            crawler.on("fetchheaders", item => {
                if (item.path === "/survivor") {
                    survivorHeadersSeen.resolve();
                }
            });
            crawler.on("fetchcomplete", (item, body, response) => {
                completions.push({ path: item.path, body: body.toString() });
                assert.ok(response instanceof http.IncomingMessage);
            });

            const complete = onceEvent(crawler, "complete");
            const timedOut = onceEvent(crawler, "fetchtimeout");

            try {
                crawler.start();
                await bothStarted.promise;
                await conditionHeld.promise;
                await survivorHeadersSeen.promise;

                survivorRequest = crawler._openRequests.find(request => request.path === "/survivor");
                const lateRequest = crawler._openRequests.find(request => request.path === "/late");

                assert.ok(survivorRequest);
                assert.ok(lateRequest);
                assert.equal(crawler._openRequests.length, 2);

                lateRequest.emit("timeout");
                const [item, timeout] = await timedOut;

                assert.equal(typeof timeout, "number");
                assert.equal(timeout, crawler.timeout);
                assert.equal(item.path, "/late");
                assert.equal(item.fetched, true);
                assert.equal(item.status, "timeout");
                assert.equal(crawler._openRequests.includes(survivorRequest), true);
                assert.equal(crawler._openRequests.includes(lateRequest), false);

                releaseCondition(null, false);
                await turn();

                assert.equal(eventsFor(records, "/late").indexOf("downloadprevented"), -1);
                assert.equal(eventsFor(records, "/late").indexOf("fetchcomplete"), -1);
                assert.equal(crawler._openRequests.includes(survivorRequest), true);

                (await survivorHeaders.promise).end("survivor body");
                await complete;

                assert.deepEqual(eventsFor(records, "/late"), ["fetchtimeout"]);
                assert.deepEqual(eventsFor(records, "/survivor"), ["fetchcomplete"]);
                assert.equal(completions.length, 1);
                assert.equal(completions[0].path, "/survivor");
                assert.equal(completions[0].body, "survivor body");
                assert.equal(queueItem(crawler, "/late").status, "timeout");
                assert.equal(queueItem(crawler, "/survivor").status, "downloaded");
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                if (lateResponse) {
                    lateResponse.destroy();
                }
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("does not deliver a timed-out gzip body when decompression completes late", async () => {
        const compressed = zlib.gzipSync(Buffer.from("decoded page"));
        const unzipStarted = deferred();
        const survivorHeaders = deferred();
        const originalUnzip = zlib.unzip;
        let releaseUnzip;
        let survivorResponse;

        zlib.unzip = (buffer, callback) => {
            releaseUnzip = () => originalUnzip(buffer, callback);
            unzipStarted.resolve();
        };

        await withServer((req, res) => {
            res.on("error", () => {});
            if (req.url === "/decompress") {
                res.writeHead(200, {
                    "Content-Type": "text/html",
                    "Content-Encoding": "gzip",
                    "Content-Length": String(compressed.length)
                });
                res.end(compressed);
            } else if (req.url === "/survivor") {
                res.writeHead(200, {"Content-Type": "text/html"});
                res.flushHeaders();
                survivorHeaders.resolve(res);
            }
        }, async base => {
            const crawler = makeCrawler(base, "/decompress", {
                maxConcurrency: 2,
                discoverResources: true
            });
            const records = recordEvents(crawler, ["fetchtimeout", "fetchcomplete", "gziperror", "fetchclienterror", "downloadprevented"]);
            const completions = [];
            const started = [];
            const bothStarted = deferred();
            const cachedPaths = [];
            const discoveredPaths = [];

            crawler.queueURL("/survivor");
            crawler.cache = {
                setCacheData(item) {
                    cachedPaths.push(item.path);
                }
            };
            crawler.queueLinkedItems = (resourceData, item) => {
                discoveredPaths.push(item.path);
            };
            crawler.on("fetchstart", item => {
                started.push(item.path);
                if (started.length === 2) {
                    bothStarted.resolve();
                }
            });
            crawler.on("fetchcomplete", (item, body) => {
                completions.push({ path: item.path, body: body.toString() });
            });

            const complete = onceEvent(crawler, "complete");
            const timedOut = onceEvent(crawler, "fetchtimeout");

            try {
                crawler.start();
                await bothStarted.promise;
                await unzipStarted.promise;
                const headers = await survivorHeaders.promise;
                survivorResponse = headers;

                const decompressRequest = crawler._openRequests.find(request => request.path === "/decompress");
                const survivorRequest = crawler._openRequests.find(request => request.path === "/survivor");

                assert.ok(decompressRequest);
                assert.ok(survivorRequest);
                assert.equal(crawler._openRequests.length, 2);

                decompressRequest.emit("timeout");
                const [item] = await timedOut;

                assert.equal(item.status, "timeout");
                assert.equal(crawler._openRequests.includes(survivorRequest), true);
                assert.equal(crawler._openRequests.includes(decompressRequest), false);

                releaseUnzip();
                await turn();
                await turn();

                assert.deepEqual(cachedPaths, []);
                assert.deepEqual(discoveredPaths, []);
                assert.equal(eventsFor(records, "/decompress").indexOf("fetchcomplete"), -1);
                assert.equal(eventsFor(records, "/decompress").indexOf("gziperror"), -1);
                assert.equal(crawler._openRequests.includes(survivorRequest), true);

                survivorResponse.end("survivor body");
                await complete;

                assert.deepEqual(eventsFor(records, "/decompress"), ["fetchtimeout"]);
                assert.deepEqual(eventsFor(records, "/survivor"), ["fetchcomplete"]);
                assert.equal(completions.length, 1);
                assert.equal(completions[0].path, "/survivor");
                assert.equal(queueItem(crawler, "/decompress").status, "timeout");
                assert.equal(queueItem(crawler, "/survivor").status, "downloaded");
                assert.deepEqual(cachedPaths, ["/survivor"]);
                assert.deepEqual(discoveredPaths, ["/survivor"]);
            } finally {
                zlib.unzip = originalUnzip;
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("honours a timeout selected while the headers write is still pending", async () => {
        const metadataHeld = deferred();
        const responseReady = deferred();
        let releaseMetadata;

        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {"Content-Type": "text/html"});
            res.flushHeaders();
            responseReady.resolve(res);
        }, async base => {
            const crawler = makeCrawler(base, "/delayed-queue");
            const originalUpdate = crawler.queue.update.bind(crawler.queue);
            const records = recordEvents(crawler, ["fetchtimeout", "fetchheaders", "fetchcomplete", "fetchdataerror", "fetchclienterror"]);
            const complete = onceEvent(crawler, "complete");
            const timedOut = onceEvent(crawler, "fetchtimeout");
            let serverResponse;

            crawler.queue.update = function (id, changes, callback) {
                if (changes.stateData && changes.stateData.headers) {
                    releaseMetadata = () => originalUpdate(id, changes, callback);
                    metadataHeld.resolve();
                    return;
                }

                originalUpdate(id, changes, callback);
            };

            try {
                crawler.start();
                await metadataHeld.promise;
                serverResponse = await responseReady.promise;

                const request = crawler._openRequests[0];

                assert.ok(request);
                assert.equal(queueItem(crawler, "/delayed-queue").status, "spooled");

                request.emit("timeout");
                await turn();

                // The final timeout write is queued behind the held headers
                // write, so no terminal notification may appear yet.
                assert.equal(eventsFor(records, "/delayed-queue").indexOf("fetchtimeout"), -1);
                assert.equal(queueItem(crawler, "/delayed-queue").status, "spooled");

                releaseMetadata();
                await timedOut;
                await complete;

                assert.deepEqual(eventsFor(records, "/delayed-queue"), ["fetchtimeout"]);
                assert.equal(queueItem(crawler, "/delayed-queue").status, "timeout");
                assert.equal(queueItem(crawler, "/delayed-queue").fetched, true);
                assert.equal(records.some(record => record.name === "fetchheaders"), false);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                if (serverResponse) {
                    serverResponse.destroy();
                }
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("settles a condition error as failed while preserving the error identity", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Length": "4"
            });
            res.end("body");
        }, async base => {
            const crawler = makeCrawler(base, "/condition-error");
            const expectedError = new Error("condition backend unavailable");
            const records = recordEvents(crawler, ["downloadconditionerror", "fetchcomplete", "fetchclienterror"]);
            const delivered = [];
            let cached = 0;
            let discovered = 0;
            const complete = onceEvent(crawler, "complete");
            const conditionError = onceEvent(crawler, "downloadconditionerror");

            crawler.addDownloadCondition((item, response, callback) => {
                setImmediate(() => callback(expectedError));
            });
            crawler.cache = {
                setCacheData() {
                    cached++;
                }
            };
            crawler.queueLinkedItems = () => {
                discovered++;
            };
            crawler.on("fetchcomplete", (item, body) => delivered.push(body.length));

            try {
                crawler.start();
                const [item, error] = await conditionError;

                assert.equal(error, expectedError);
                assert.equal(item.fetched, true);
                assert.equal(item.status, "failed");
                assert.equal(delivered.length, 0);
                assert.equal(cached, 0);
                assert.equal(discovered, 0);
                assert.equal(records.filter(record => record.name === "downloadconditionerror").length, 1);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(crawler._openRequests.length, 0);

                await complete;
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("keeps a normal download rejection on the downloadprevented path", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Length": "4"
            });
            res.end("body");
        }, async base => {
            const crawler = makeCrawler(base, "/condition-prevented");
            const records = recordEvents(crawler, ["downloadprevented", "fetchcomplete", "downloadconditionerror", "fetchclienterror"]);
            const delivered = [];
            const complete = onceEvent(crawler, "complete");
            const prevented = onceEvent(crawler, "downloadprevented");

            crawler.addDownloadCondition((item, response, callback) => {
                setImmediate(() => callback(null, false));
            });
            crawler.on("fetchcomplete", (item, body) => delivered.push(body.length));

            try {
                crawler.start();
                const [item, response] = await prevented;

                assert.ok(response instanceof http.IncomingMessage);
                assert.equal(item.fetched, true);
                assert.equal(item.status, "downloadprevented");
                assert.equal(delivered.length, 0);
                assert.equal(records.filter(record => record.name === "downloadprevented").length, 1);
                assert.equal(records.some(record => record.name === "fetchcomplete"), false);
                assert.equal(records.some(record => record.name === "downloadconditionerror"), false);
                assert.equal(crawler._openRequests.length, 0);

                await complete;
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("delivers a valid gzip body once, decoded", async () => {
        const body = "decoded page";
        const compressed = zlib.gzipSync(Buffer.from(body));

        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Encoding": "gzip",
                "Content-Length": String(compressed.length)
            });
            res.end(compressed);
        }, async base => {
            const crawler = makeCrawler(base, "/gzip");
            const records = recordEvents(crawler, ["gziperror", "fetchcomplete", "fetchclienterror", "fetchdataerror"]);
            const delivered = [];
            const complete = onceEvent(crawler, "complete");

            crawler.on("fetchcomplete", (item, responseBody, response) => {
                assert.ok(response instanceof http.IncomingMessage);
                delivered.push(responseBody.toString());
            });

            try {
                crawler.start();
                await complete;

                assert.equal(delivered.length, 1);
                assert.equal(delivered[0], body);
                assert.equal(queueItem(crawler, "/gzip").status, "downloaded");
                assert.equal(records.some(record => record.name === "gziperror"), false);
                assert.equal(records.some(record => record.name === "fetchclienterror"), false);
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("delivers the raw bytes once after gziperror, keeping gziperror diagnostic", async () => {
        const raw = Buffer.from("this is not gzip");

        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Encoding": "gzip",
                "Content-Length": String(raw.length)
            });
            res.end(raw);
        }, async base => {
            const crawler = makeCrawler(base, "/gzip-error");
            const records = recordEvents(crawler, ["gziperror", "fetchcomplete", "fetchclienterror", "fetchdataerror"]);
            const delivered = [];
            const complete = onceEvent(crawler, "complete");

            crawler.on("fetchcomplete", (item, responseBody) => {
                delivered.push(Buffer.isBuffer(responseBody) ? responseBody : Buffer.from(responseBody));
            });

            try {
                crawler.start();
                await complete;

                const gzipErrors = records.filter(record => record.name === "gziperror");

                assert.equal(gzipErrors.length, 1);
                assert.equal(gzipErrors[0].value instanceof Error, true);
                assert.deepEqual(Buffer.from(gzipErrors[0].extra), raw);
                assert.equal(delivered.length, 1);
                assert.equal(Buffer.compare(delivered[0], raw), 0);
                assert.equal(queueItem(crawler, "/gzip-error").status, "downloaded");
                assert.equal(records.some(record => record.name === "fetchclienterror"), false);
                assert.equal(records.some(record => record.name === "fetchdataerror"), false);
                assert.equal(crawler._openRequests.length, 0);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("settles successfully with the synchronous default queue backend", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Length": "9"
            });
            res.end("sync body");
        }, async base => {
            const crawler = makeCrawler(base, "/sync-queue");
            const records = recordEvents(crawler, ["fetchcomplete", "fetchclienterror", "fetchdataerror"]);
            const delivered = [];
            const complete = onceEvent(crawler, "complete");

            crawler.on("fetchcomplete", (item, body) => delivered.push(body.toString()));

            try {
                crawler.start();
                await complete;

                assert.equal(delivered.length, 1);
                assert.equal(delivered[0], "sync body");
                assert.equal(queueItem(crawler, "/sync-queue").status, "downloaded");
                assert.equal(queueItem(crawler, "/sync-queue").fetched, true);
                assert.equal(crawler._openRequests.length, 0);
                assert.equal(records.some(record => record.name === "fetchclienterror"), false);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });

    await t.test("settles successfully with a delayed queue.update backend", async () => {
        await withServer((req, res) => {
            res.on("error", () => {});
            res.writeHead(200, {
                "Content-Type": "text/html",
                "Content-Length": "13"
            });
            res.end("delayed body!");
        }, async base => {
            const crawler = makeCrawler(base, "/delayed-update");
            const originalUpdate = crawler.queue.update.bind(crawler.queue);
            const records = recordEvents(crawler, ["fetchcomplete", "fetchclienterror", "fetchdataerror"]);
            const delivered = [];
            const complete = onceEvent(crawler, "complete");

            crawler.queue.update = function (id, changes, callback) {
                setImmediate(() => originalUpdate(id, changes, callback));
            };
            crawler.on("fetchcomplete", (item, body) => delivered.push(body.toString()));

            try {
                crawler.start();
                await complete;

                assert.equal(delivered.length, 1);
                assert.equal(delivered[0], "delayed body!");
                assert.equal(queueItem(crawler, "/delayed-update").status, "downloaded");
                assert.equal(queueItem(crawler, "/delayed-update").fetched, true);
                assert.equal(crawler._openRequests.length, 0);
                assert.equal(records.some(record => record.name === "fetchclienterror"), false);
            } finally {
                disposeCrawler(crawler);
            }
        });
    });
});
