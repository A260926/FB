'use strict'
const assert = require('node:assert/strict')
const Xray = require('..')
let calls = 0
const x = Xray({ filters: { later: value => Promise.resolve(value.toUpperCase()) } })
x.driver((ctx, done) => { calls++; ctx.body = '<h1>alpha</h1>'; done(null, ctx) })
x('http://local.test/page', { title: 'h1 | later' })((err, data) => {
  if (err) throw err
  assert.equal(calls, 1)
  assert.equal(typeof data.title.then, 'function')
  console.log('XRAY_DRIVER_CALLS=1')
  console.log('UNAWAITED_FIELD=' + JSON.stringify(data))
  data.title.then(value => { assert.equal(value, 'ALPHA'); console.log('FILTER_EVENTUAL_VALUE=ALPHA') })
})
