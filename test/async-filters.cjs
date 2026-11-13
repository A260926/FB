'use strict'

const { describe, it, after } = require('node:test')
const assert = require('node:assert/strict')
const path = require('node:path')
const fs = require('node:fs')
const os = require('node:os')
const Xray = require('..')

/**
 * A promise released by hand from the test.
 */
function gate () {
  let resolve, reject
  const promise = new Promise((res, rej) => { resolve = res; reject = rej })
  return { promise, resolve, reject }
}

/**
 * Flush microtasks and one macrotask turn.
 */
function tick () {
  return new Promise(resolve => setImmediate(resolve))
}

/**
 * Local driver: serves `pages` and records the
 * actual request order in `requests`.
 */
function drive (pages, requests) {
  return function driver (ctx, done) {
    requests.push(ctx.url)
    if (!(ctx.url in pages)) return done(new Error('unexpected request: ' + ctx.url))
    ctx.body = pages[ctx.url]
    done(null, ctx)
  }
}

describe('Async values', () => {
  it('runs chains in order and assembles fields completing out of order', async () => {
    const gateA = gate()
    const gateB = gate()
    const tagged = []
    const contexts = []
    const x = Xray({
      filters: {
        waitA: function (value) {
          contexts.push({
            selector: this.selector,
            attribute: this.attribute,
            has$: typeof this.$ === 'function'
          })
          return gateA.promise.then(() => value.toUpperCase())
        },
        waitB: function (value) {
          return gateB.promise.then(() => value.toUpperCase())
        },
        tag: function (value, mark) {
          tagged.push(typeof value) // the settled string, not the promise
          return value + mark
        }
      }
    })

    const done = new Promise((resolve, reject) => {
      x('<h1>alpha</h1><h2>beta</h2>', {
        a: 'h1 | waitA | tag:!',
        b: 'h2 | waitB'
      })((err, data) => err ? reject(err) : resolve(data))
    })

    await tick()
    gateB.resolve() // finish the second field first
    await tick()
    gateA.resolve()
    const data = await done

    assert.deepEqual(data, { a: 'ALPHA!', b: 'BETA' })
    assert.deepEqual(Object.keys(data), ['a', 'b'])
    assert.deepEqual(tagged, ['string'])
    assert.deepEqual(contexts, [{ selector: 'h1', attribute: 'text', has$: true }])
    assert.equal(typeof data.a.then, 'undefined')
  })

  it('adopts thenables exactly once and keeps the first settlement', async () => {
    let gets = 0
    let calls = 0
    const weird = {}
    Object.defineProperty(weird, 'then', {
      get () {
        gets++
        return function (resolve, reject) {
          calls++
          resolve('first')
          resolve('second')
          reject(new Error('third'))
        }
      }
    })

    const x = Xray({
      filters: {
        thenable: () => weird,
        plain: () => ({ then: 'not-a-function' })
      }
    })

    const data = await new Promise((resolve, reject) => {
      x('<h1>a</h1><p>b</p>', {
        t: 'h1 | thenable',
        o: 'p | plain'
      })((err, d) => err ? reject(err) : resolve(d))
    })

    assert.equal(data.t, 'first')
    assert.deepEqual(data.o, { then: 'not-a-function' })
    assert.equal(gets, 1)
    assert.equal(calls, 1)
  })

  it('runs each array element through its own chain and reassembles in order', async () => {
    const gates = { a: gate(), b: gate(), c: gate() }
    const x = Xray({
      filters: {
        wait: value => gates[value].promise.then(() => value.toUpperCase())
      }
    })

    const done = new Promise((resolve, reject) => {
      x('<ul><li>a</li><li>b</li><li>c</li></ul>', ['li | wait'])((err, data) => {
        err ? reject(err) : resolve(data)
      })
    })

    await tick()
    gates.c.resolve() // release the elements in reverse
    gates.b.resolve()
    gates.a.resolve()
    const data = await done

    assert.deepEqual(data, ['A', 'B', 'C'])
  })

  it('keeps the omission and compact rules for settled values', async () => {
    const x = Xray({
      filters: {
        empty: () => Promise.resolve(''),
        nothing: () => Promise.resolve(undefined),
        zero: () => Promise.resolve(0),
        no: () => Promise.resolve(false),
        obj: value => Promise.resolve({ raw: value })
      }
    })

    const data = await new Promise((resolve, reject) => {
      x('<h1>x</h1><h2>y</h2><h3>z</h3><h4>w</h4><h5>v</h5>', {
        keep: 'h1 | obj',
        dropEmpty: 'h2 | empty',
        dropUndef: 'h3 | nothing',
        zero: 'h4 | zero',
        no: 'h5 | no'
      })((err, d) => err ? reject(err) : resolve(d))
    })

    // '' and undefined are omitted, plain objects, 0 and false survive
    assert.deepEqual(data, { keep: { raw: 'x' }, zero: 0, no: false })
  })

  it('compacts array rows whose settled fields are all omitted', async () => {
    const x = Xray({
      filters: {
        drop: value => Promise.resolve(value === 'b' ? '' : value)
      }
    })

    const data = await new Promise((resolve, reject) => {
      x('<ul><li><b>a</b></li><li><b>b</b></li><li><b>c</b></li></ul>', 'li', [{
        name: 'b | drop'
      }])((err, d) => err ? reject(err) : resolve(d))
    })

    assert.deepEqual(data, [{ name: 'a' }, { name: 'c' }])
  })

  it('keeps synchronous chains synchronous and callbacks on the nextTick path', async () => {
    const events = []
    const x = Xray({
      filters: {
        up: value => { events.push('filter'); return value.toUpperCase() }
      }
    })

    // invoke from a macrotask so the nextTick/microtask
    // interleaving matches a plain top-level script
    const data = await new Promise((resolve, reject) => {
      setImmediate(() => {
        x('<h1>old</h1>', { name: 'h1 | up' })((err, data) => {
          if (err) return reject(err)
          events.push('callback')
          resolve(data)
        })
        events.push('invoked')

        // the filter ran synchronously, the outer callback did not
        assert.deepEqual(events, ['filter', 'invoked'])

        process.nextTick(() => events.push('tick'))
        Promise.resolve().then(() => events.push('micro'))
      })
    })

    assert.deepEqual(data, { name: 'OLD' })
    // the callback keeps Batch's nextTick path: after the
    // synchronous body and the next nextTick, before
    // promise microtasks
    assert.deepEqual(events, ['filter', 'invoked', 'tick', 'callback', 'micro'])
  })

  it('delivers settled data through callback, stream, write and then', async () => {
    const html = '<h1>alpha</h1>'
    const selector = { title: 'h1 | wait' }
    const expected = { title: 'ALPHA' }
    const make = () => {
      const g = gate()
      const x = Xray({
        filters: { wait: value => g.promise.then(() => value.toUpperCase()) }
      })
      return { g, x }
    }

    // callback
    const a = make()
    const viaCallback = new Promise((resolve, reject) => {
      a.x(html, selector)((err, data) => err ? reject(err) : resolve(data))
    })
    a.g.resolve()

    // stream
    const b = make()
    const rs = b.x(html, selector).stream()
    let text = ''
    rs.on('data', chunk => { text += chunk })
    const viaStream = new Promise((resolve, reject) => {
      rs.on('end', () => resolve(text))
      rs.on('error', reject)
    })
    await tick()
    assert.equal(text, '') // JSON output is not closed before filters settle
    b.g.resolve()

    // write
    const c = make()
    const file = path.join(os.tmpdir(), `xray-async-${process.pid}.json`)
    const ws = c.x(html, selector).write(file)
    const viaWrite = new Promise((resolve, reject) => {
      ws.on('finish', () => resolve(fs.readFileSync(file, 'utf8')))
      ws.on('error', reject)
    })
    c.g.resolve()

    // then
    const d = make()
    const viaThen = d.x(html, selector).then()
    d.g.resolve()

    const [cbData, streamText, fileText, thenData] = await Promise.all([viaCallback, viaStream, viaWrite, viaThen])
    try {
      assert.deepEqual(cbData, expected)
      assert.deepEqual(JSON.parse(streamText), expected)
      assert.deepEqual(JSON.parse(fileText), expected)
      assert.deepEqual(thenData, expected)
    } finally {
      fs.unlinkSync(file)
    }
  })
})

describe('Request selectors', () => {
  it('waits for filtered detail urls before requesting', async () => {
    const requests = []
    const g = gate()
    const pages = {
      'http://local.test/list': '<a class="d" href="/detail-1">go</a>',
      'http://local.test/detail-2': '<h1>deep</h1>'
    }
    const x = Xray({
      filters: {
        lookup: value => g.promise.then(() => ({
          'http://local.test/detail-1': 'http://local.test/detail-2'
        }[value]))
      }
    })
    x.driver(drive(pages, requests))

    const done = new Promise((resolve, reject) => {
      x('http://local.test/list', {
        detail: x('a.d@href | lookup', { title: 'h1' })
      })((err, data) => err ? reject(err) : resolve(data))
    })

    await tick()
    await tick()
    // the detail request waits for the filtered url
    assert.deepEqual(requests, ['http://local.test/list'])
    g.resolve()
    const data = await done

    assert.deepEqual(requests, ['http://local.test/list', 'http://local.test/detail-2'])
    assert.deepEqual(data, { detail: { title: 'deep' } })
  })

  it('waits for filtered paginate urls and keeps abort, limit and order', async () => {
    const requests = []
    const g = gate()
    const pages = {
      'http://local.test/p1': '<ul><li><b>a</b></li></ul><a class="next" href="/p2">next</a>',
      'http://local.test/p2': '<ul><li><b>b</b></li></ul><a class="next" href="/p3">next</a>',
      'http://local.test/p3': '<ul><li><b>c</b></li></ul>'
    }
    const aborted = []
    const x = Xray({
      filters: {
        lookup: value => value === undefined ? undefined : g.promise.then(() => value)
      }
    })
    x.driver(drive(pages, requests))

    const done = new Promise((resolve, reject) => {
      x('http://local.test/p1', 'li', [{ name: 'b' }])
        .paginate('a.next@href | lookup')
        .abort((page, url) => { aborted.push([page, url]); return false })
        .limit(2)((err, data) => err ? reject(err) : resolve(data))
    })

    await tick()
    await tick()
    // the next page request waits for the filtered url
    assert.deepEqual(requests, ['http://local.test/p1'])
    g.resolve()
    const data = await done

    // the page limit stops the crawl before p3
    assert.deepEqual(requests, ['http://local.test/p1', 'http://local.test/p2'])
    assert.deepEqual(data, [{ name: 'a' }, { name: 'b' }])
    // abort saw the settled page records and the settled next url
    assert.deepEqual(aborted, [[[{ name: 'a' }], 'http://local.test/p2']])
  })

  it('finishes when the filtered paginate url is missing', async () => {
    const requests = []
    const pages = {
      'http://local.test/p1': '<ul><li><b>a</b></li></ul><a class="next" href="/p2">next</a>',
      'http://local.test/p2': '<ul><li><b>b</b></li></ul>'
    }
    const x = Xray({
      filters: {
        lookup: value => value === undefined ? undefined : Promise.resolve(value)
      }
    })
    x.driver(drive(pages, requests))

    const data = await new Promise((resolve, reject) => {
      x('http://local.test/p1', 'li', [{ name: 'b' }])
        .paginate('a.next@href | lookup')
        .limit(5)((err, d) => err ? reject(err) : resolve(d))
    })

    assert.deepEqual(requests, ['http://local.test/p1', 'http://local.test/p2'])
    assert.deepEqual(data, [{ name: 'a' }, { name: 'b' }])
  })
})

describe('Output errors', () => {
  const unhandled = []
  const onUnhandled = reason => { unhandled.push(reason) }
  process.on('unhandledRejection', onUnhandled)

  after(async () => {
    await tick()
    process.removeListener('unhandledRejection', onUnhandled)
    assert.deepEqual(unhandled, [])
  })

  it('ends output once with the original error when a filter throws synchronously', async () => {
    const original = new Error('sync-boom')
    let spy = 0
    let calls = 0
    const x = Xray({
      filters: {
        bad: () => { throw original },
        spy: value => { spy++; return value }
      }
    })

    const first = new Promise(resolve => {
      x('<h1>a</h1>', { t: 'h1 | bad | spy' })(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original)
    assert.equal(spy, 0) // later filters never ran
    await tick()
    assert.equal(calls, 1)
  })

  it('ends output once with the original error when a promise rejects', async () => {
    const original = new Error('promise-boom')
    let spy = 0
    let calls = 0
    const x = Xray({
      filters: {
        bad: () => Promise.reject(original),
        spy: value => { spy++; return value }
      }
    })

    const first = new Promise(resolve => {
      x('<h1>a</h1>', { t: 'h1 | bad | spy' })(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original)
    assert.equal(spy, 0)
    await tick()
    assert.equal(calls, 1)
  })

  it('ends output once with the original error when a then getter throws', async () => {
    const original = new Error('getter-boom')
    let calls = 0
    const x = Xray({
      filters: {
        bad: () => Object.defineProperty({}, 'then', { get () { throw original } })
      }
    })

    const first = new Promise(resolve => {
      x('<h1>a</h1>', { t: 'h1 | bad' })(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original)
    await tick()
    assert.equal(calls, 1)
  })

  it('ends output once with the original error when a thenable rejects', async () => {
    const original = new Error('thenable-boom')
    let calls = 0
    const x = Xray({
      filters: {
        bad: () => ({
          then (resolve, reject) {
            reject(original)
            resolve('late')
            reject(new Error('later'))
          }
        })
      }
    })

    const first = new Promise(resolve => {
      x('<h1>a</h1>', { t: 'h1 | bad' })(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original) // only the first settlement counts
    await tick()
    assert.equal(calls, 1)
  })

  it('does not call back again when siblings settle late', async () => {
    const original = new Error('first-boom')
    const late = new Error('late-boom')
    const gateA = gate()
    const gateB = gate()
    let calls = 0
    const x = Xray({
      filters: {
        failA: () => gateA.promise.then(() => { throw original }),
        failB: () => gateB.promise.then(() => { throw late })
      }
    })

    const first = new Promise(resolve => {
      x('<h1>a</h1><h2>b</h2>', {
        a: 'h1 | failA',
        b: 'h2 | failB'
      })(err => { calls++; resolve(err) })
    })

    gateA.resolve()
    const err = await first
    assert.strictEqual(err, original)

    gateB.resolve() // the sibling finishes after the output ended
    await tick()
    await tick()
    assert.equal(calls, 1)
  })

  it('stops paginating when a field filter fails', async () => {
    const requests = []
    const original = new Error('page-boom')
    const pages = {
      'http://local.test/p1': '<ul><li><b>a</b></li></ul><a class="next" href="/p2">next</a>',
      'http://local.test/p2': '<ul><li><b>b</b></li></ul>'
    }
    const x = Xray({
      filters: { bad: () => Promise.reject(original) }
    })
    x.driver(drive(pages, requests))

    let calls = 0
    const first = new Promise(resolve => {
      x('http://local.test/p1', 'li', [{ name: 'b | bad' }])
        .paginate('a.next@href')
        .limit(5)(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original)
    await tick()
    assert.deepEqual(requests, ['http://local.test/p1'])
    assert.equal(calls, 1)
  })

  it('ends output once when a paginate filter rejects', async () => {
    const requests = []
    const original = new Error('paginate-boom')
    const pages = {
      'http://local.test/p1': '<ul><li><b>a</b></li></ul><a class="next" href="/p2">next</a>',
      'http://local.test/p2': '<ul><li><b>b</b></li></ul>'
    }
    const x = Xray({
      filters: { bad: () => Promise.reject(original) }
    })
    x.driver(drive(pages, requests))

    let calls = 0
    const first = new Promise(resolve => {
      x('http://local.test/p1', 'li', [{ name: 'b' }])
        .paginate('a.next@href | bad')
        .limit(5)(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original)
    await tick()
    assert.deepEqual(requests, ['http://local.test/p1'])
    assert.equal(calls, 1)
  })

  it('ends output once when a detail url filter rejects', async () => {
    const requests = []
    const original = new Error('detail-boom')
    const pages = {
      'http://local.test/list': '<a class="d" href="/detail-1">go</a>'
    }
    const x = Xray({
      filters: { bad: () => Promise.reject(original) }
    })
    x.driver(drive(pages, requests))

    let calls = 0
    const first = new Promise(resolve => {
      x('http://local.test/list', {
        detail: x('a.d@href | bad', { title: 'h1' })
      })(err => { calls++; resolve(err) })
    })
    const err = await first

    assert.strictEqual(err, original)
    await tick()
    assert.deepEqual(requests, ['http://local.test/list'])
    assert.equal(calls, 1)
  })

  it('emits the original error on the stream, write and then paths', async () => {
    const original = new Error('output-boom')
    const make = () => Xray({
      filters: { bad: () => Promise.reject(original) }
    })

    // stream
    const rs = make()('<h1>a</h1>', { t: 'h1 | bad' }).stream()
    const streamErr = await new Promise(resolve => {
      rs.on('data', () => {})
      rs.on('error', resolve)
    })
    assert.strictEqual(streamErr, original)

    // write
    const file = path.join(os.tmpdir(), `xray-async-err-${process.pid}.json`)
    const ws = make()('<h1>a</h1>', { t: 'h1 | bad' }).write(file)
    const writeErr = await new Promise(resolve => ws.on('error', resolve))
    assert.strictEqual(writeErr, original)
    ws.destroy()
    try { fs.unlinkSync(file) } catch (e) { /* may not have been created */ }

    // then
    await assert.rejects(
      make()('<h1>a</h1>', { t: 'h1 | bad' }).then(),
      err => err === original
    )
  })
})
