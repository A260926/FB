"use strict"

const assert = require("node:assert/strict")
const http = require("node:http")
const test = require("node:test")
const cheerio = require("cheerio")
const scrapeIt = require("..")

/**
 * Router for the loopback fixture server. It records every request path so
 * the assertions below can check the exact request order, query strings and
 * fragment stripping.
 */
function createRouter (state) {
    return function router (req, res) {
        state.seen.push(req.url)
        res.setHeader("Content-Type", "text/html; charset=utf-8")

        const path = req.url.split("?")[0].split("#")[0]

        if (req.url === "/start") {
            res.statusCode = 302
            res.setHeader("Location", "/dir")
            return res.end()
        }

        if (path === "/dir") {
            res.statusCode = 200
            return res.end(`
                <article>
                    <b>A</b>
                    <a class="detail" href="items/a?x=1#part">go</a>
                </article>
                <article>
                    <b>B</b>
                    <a class="detail" href="/items/shared">go</a>
                </article>
                <article>
                    <b>C</b>
                    <a class="detail" href="/items/shared">go</a>
                </article>
                <article><b>D</b></article>
                <article>
                    <b>E</b>
                    <a class="detail" href="mailto:x@example.com">mail</a>
                </article>
                <article>
                    <b>F</b>
                    <a class="detail" href="">empty</a>
                </article>
                <article>
                    <b>G</b>
                    <a class="detail" href="   ">blank</a>
                </article>
                <div class="featured">
                    <a href="/items/featured">featured</a>
                </div>
            `)
        }

        if (path === "/items/a") {
            res.statusCode = 200
            return res.end(`
                <h1>A title</h1>
                <span class="tag">t1</span>
                <span class="tag">t2</span>
                <a class="more" href="a-more">more</a>
            `)
        }
        if (path === "/items/a-more") {
            res.statusCode = 200
            return res.end("<h1>A more</h1>")
        }

        if (path === "/items/shared") {
            state.sharedHits++
            res.statusCode = 200
            return res.end("<h1>Shared</h1>")
        }

        if (path === "/items/featured") {
            res.statusCode = 200
            return res.end("<h1>Featured</h1>")
        }

        if (path === "/boom") {
            res.statusCode = 500
            return res.end("<h1>Boom</h1>")
        }

        if (path === "/fail-dir") {
            res.statusCode = 200
            return res.end(`
                <article><a class="detail" href="/boom">broken</a></article>
            `)
        }

        if (path === "/loop-dir") {
            res.statusCode = 200
            return res.end(`
                <article><a class="detail" href="/loop-dir">loop</a></article>
            `)
        }

        if (path === "/redir-loop") {
            res.statusCode = 200
            return res.end(`
                <article><a class="detail" href="/redir-step">step</a></article>
            `)
        }

        if (path === "/redir-step") {
            res.statusCode = 302
            res.setHeader("Location", "/redir-loop")
            return res.end()
        }

        res.statusCode = 404
        res.end("not found")
    }
}

function deepFreeze (value) {
    if (value !== null && typeof value === "object") {
        for (const key of Object.keys(value)) {
            deepFreeze(value[key])
        }
        Object.freeze(value)
    }
    return value
}

async function rejectError (promise) {
    let caught
    await assert.rejects(promise, err => {
        caught = err
        return true
    })
    return caught
}

test("Linked extraction", async t => {
    const state = { seen: [], sharedHits: 0 }
    const server = http.createServer(createRouter(state))

    await new Promise((resolve, reject) => {
        server.once("error", reject)
        server.listen(0, "127.0.0.1", resolve)
    })

    const base = `http://127.0.0.1:${server.address().port}`
    const seenSince = offset => state.seen.slice(offset)

    // A reused, deeply frozen schema: the implementation must never write into
    // it, and two independent calls must not share DOMs, ancestors or results.
    const schema = deepFreeze({
        rows: {
            listItem: "article"
          , data: {
                name: "b"
              , detail: {
                    selector: "a.detail"
                  , attr: "href"
                  , follow: {
                        data: {
                            title: "h1"
                          , tags: { listItem: ".tag" }
                          , missing: ".does-not-exist"
                          , more: {
                                selector: "a.more"
                              , follow: { data: { title: "h1" } }
                            }
                        }
                    }
                }
            }
        }
      , featured: {
            selector: ".featured"
          , data: {
                detail: {
                    selector: "a"
                  , follow: { data: { title: "h1" } }
                  , convert: (value, $node) => Object.assign({}, value, {
                        linkHref: $node.attr("href")
                    })
                }
            }
        }
    })

    const expectedData = {
        rows: [
            {
                name: "A"
              , detail: {
                    title: "A title"
                  , tags: ["t1", "t2"]
                  , missing: ""
                  , more: { title: "A more" }
                }
            }
          , {
                name: "B"
              , detail: { title: "Shared", tags: [], missing: "", more: null }
            }
          , {
                name: "C"
              , detail: { title: "Shared", tags: [], missing: "", more: null }
            }
          , { name: "D", detail: null }
          , { name: "E", detail: null }
          , { name: "F", detail: null }
          , { name: "G", detail: null }
        ]
      , featured: {
            detail: { title: "Featured", linkHref: "/items/featured" }
        }
    }

    await t.test("follows links through a catalog redirect, with exact request order", async () => {
        const offset = state.seen.length
        const result = await scrapeIt(`${base}/start`, schema)

        assert.deepEqual(result.data, expectedData)

        // Catalog redirect, relative second-level link, shared sibling URL
        // requested twice, query kept, fragment dropped, missing links silent.
        assert.deepEqual(seenSince(offset), [
            "/start"
          , "/dir"
          , "/items/a?x=1"
          , "/items/a-more"
          , "/items/shared"
          , "/items/shared"
          , "/items/featured"
        ])
        assert.equal(state.sharedHits, 2)

        // The root response metadata still describes the root page.
        assert.equal(result.status, 200)
        assert.equal(typeof result.$, "function")
        assert.equal(result.$("article").length, 7)
        assert.ok(result.body.includes("<article>"))
        assert.ok(result.request.res.responseUrl.endsWith("/dir"))
    })

    await t.test("detail objects of sibling records are never shared", async () => {
        const result = await scrapeIt(`${base}/dir`, schema)
        const rows = result.data.rows

        assert.deepEqual(rows[1].detail, rows[2].detail)
        assert.notStrictEqual(rows[1].detail, rows[2].detail)
        assert.notStrictEqual(rows[1].detail.tags, rows[2].detail.tags)
    })

    await t.test("independent calls do not share state or frozen schemas", async () => {
        const first = await scrapeIt(`${base}/dir`, schema)
        const second = await scrapeIt(`${base}/dir`, schema)

        assert.deepEqual(first.data, expectedData)
        assert.deepEqual(second.data, expectedData)
        assert.notStrictEqual(
            first.data.rows[1].detail
          , second.data.rows[1].detail
        )
        // The frozen schema must still be exactly as the caller defined it.
        assert.deepEqual(Object.keys(schema), ["rows", "featured"])
        assert.ok(Object.isFrozen(schema.rows.data.detail))
    })

    await t.test("schemas without follow keep returning raw values", async () => {
        const offset = state.seen.length
        const { data } = await scrapeIt(`${base}/dir`, {
            rows: {
                listItem: "article"
              , data: {
                    name: "b"
                  , detail: { selector: "a.detail", attr: "href" }
                }
            }
        })

        assert.deepEqual(data.rows[0], { name: "A", detail: "items/a?x=1#part" })
        assert.deepEqual(data.rows[3], { name: "D", detail: "" })
        // No detail requests were made.
        assert.deepEqual(seenSince(offset), ["/dir"])
    })

    await t.test("an HTTP failure rejects the whole call with the field path", async () => {
        const offset = state.seen.length
        const err = await rejectError(scrapeIt(`${base}/fail-dir`, {
            rows: {
                listItem: "article"
              , data: {
                    detail: {
                        selector: "a.detail"
                      , follow: { data: { title: "h1" } }
                    }
                }
            }
        }))

        assert.equal(err.field, "rows[0].detail")
        assert.ok(err.cause instanceof Error)
        assert.match(err.message, /rows\[0\]\.detail/)
        // The failing page itself was requested, nothing got published.
        assert.deepEqual(seenSince(offset), ["/fail-dir", "/boom"])
    })

    await t.test("a link back to an ancestor is rejected before requesting it", async () => {
        const offset = state.seen.length
        const err = await rejectError(scrapeIt(`${base}/loop-dir`, {
            rows: {
                listItem: "article"
              , data: {
                    detail: {
                        selector: "a.detail"
                      , follow: { data: { title: "h1" } }
                    }
                }
            }
        }))

        assert.equal(err.code, "FOLLOW_CYCLE")
        assert.equal(err.field, "rows[0].detail")
        // Only the page itself was requested; the loop link never left.
        assert.deepEqual(seenSince(offset), ["/loop-dir"])
    })

    await t.test("a redirect back to an ancestor is rejected after the response", async () => {
        const offset = state.seen.length
        const err = await rejectError(scrapeIt(`${base}/redir-loop`, {
            rows: {
                listItem: "article"
              , data: {
                    detail: {
                        selector: "a.detail"
                      , follow: { data: { title: "h1" } }
                    }
                }
            }
        }))

        assert.equal(err.code, "FOLLOW_CYCLE")
        assert.equal(err.field, "rows[0].detail")
        // The redirect hop reaches the server, then extraction is refused.
        assert.deepEqual(seenSince(offset), [
            "/redir-loop"
          , "/redir-step"
          , "/redir-loop"
        ])
    })

    await t.test("how/texteq combined with follow fail before any request", async () => {
        const offset = state.seen.length

        const howErr = await rejectError(scrapeIt(`${base}/dir`, {
            x: {
                selector: "a"
              , how: "text"
              , follow: { data: { title: "h1" } }
            }
        }))
        assert.equal(howErr.code, "FOLLOW_CONFIG_INVALID")

        const texteqErr = await rejectError(scrapeIt(`${base}/dir`, {
            x: {
                selector: "p"
              , texteq: 0
              , follow: { data: { title: "h1" } }
            }
        }))
        assert.equal(texteqErr.code, "FOLLOW_CONFIG_INVALID")

        assert.deepEqual(seenSince(offset), [])
    })

    await t.test("synchronous scrapeHTML rejects follow schemas without touching the network", async () => {
        const $ = cheerio.load('<a href="/x">t</a>')
        const followSchema = {
            x: { selector: "a", follow: { data: { title: "h1" } } }
        }

        assert.throws(() => scrapeIt.scrapeHTML($, followSchema), err =>
            err.code === "ASYNC_FOLLOW_REQUIRED"
        )

        // Ordinary synchronous usage is unchanged.
        assert.deepEqual(scrapeIt.scrapeHTML($, { x: "a" }), { x: "t" })
    })

    await t.test("a field named follow without a directive stays a normal field", async () => {
        const offset = state.seen.length
        const { data } = await scrapeIt(`${base}/dir`, {
            rows: {
                listItem: "article"
              , data: {
                    name: "b"
                  , follow: "b"
                }
            }
        })

        // A string-shorthand field named "follow" is not mistaken for a directive.
        assert.equal(data.rows[0].name, "A")
        assert.equal(data.rows[0].follow, "A")
        // No detail requests were made.
        assert.deepEqual(seenSince(offset), ["/dir"])

        // An object-valued follow without a data schema fails configuration.
        const bad = await rejectError(scrapeIt(`${base}/dir`, {
            x: { selector: "a", follow: {} }
        }))
        assert.equal(bad.code, "FOLLOW_CONFIG_INVALID")
        assert.deepEqual(seenSince(offset), ["/dir"])
    })

    await new Promise(resolve => server.close(resolve))
})
