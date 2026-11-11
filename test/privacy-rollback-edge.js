'use strict'

// Boundary regressions for privacy rollback: mid-flight errors, consecutive
// reuse, nested invocation failure captured by the outer serializer, and
// fields whose original value is `undefined`.
// Run with: node test/privacy-rollback-edge.js

const assert = require('node:assert/strict')
const fastRedact = require('..')

let failures = 0
function check (name, run) {
  try {
    run()
    console.log(name + ': PASS')
  } catch (e) {
    failures++
    console.log(name + ': FAIL ' + e.message)
  }
}

// a custom censor that throws part way through must leave every field it
// already touched restored, rethrow the original error object, and the
// censor must not be retried (static path)
check('censor throws mid static paths', () => {
  const error = new Error('sample failure')
  let calls = 0
  const redact = fastRedact({
    paths: ['token', 'email', 'count'],
    censor (value) {
      calls++
      if (calls === 2) throw error
      return '[MASKED]'
    },
    serialize: false
  })
  const o = { token: 'PUBLIC_SAMPLE_A', email: 'demo@example.invalid', count: 2 }
  assert.throws(() => redact(o), (e) => e === error)
  assert.equal(calls, 2)
  assert.equal(o.token, 'PUBLIC_SAMPLE_A')
  assert.equal(o.email, 'demo@example.invalid')
  assert.equal(o.count, 2)
})

// same guarantee for a trailing wildcard (group redaction rolls back the
// keys it already censored before the error reached the caller)
check('censor throws mid trailing wildcard', () => {
  const error = new Error('sample failure')
  let calls = 0
  const redact = fastRedact({
    paths: ['payload.*'],
    censor (value) {
      calls++
      if (calls === 2) throw error
      return '[MASKED]'
    },
    serialize: false
  })
  const o = { payload: { token: 'PUBLIC_SAMPLE_A', email: 'demo@example.invalid' } }
  assert.throws(() => redact(o), (e) => e === error)
  assert.equal(calls, 2)
  assert.equal(o.payload.token, 'PUBLIC_SAMPLE_A')
  assert.equal(o.payload.email, 'demo@example.invalid')
})

// same guarantee for a middle wildcard (nested redaction restores the
// instructions recorded before the censor threw)
check('censor throws mid intermediate wildcard', () => {
  const error = new Error('sample failure')
  let calls = 0
  const redact = fastRedact({
    paths: ['rows.*.email'],
    censor (value) {
      calls++
      if (calls === 2) throw error
      return '[MASKED]'
    },
    serialize: false
  })
  const o = { rows: [{ email: 'demo@example.invalid' }, { email: 'demo@example.invalid' }] }
  assert.throws(() => redact(o), (e) => e === error)
  assert.equal(calls, 2)
  assert.equal(o.rows[0].email, 'demo@example.invalid')
  assert.equal(o.rows[1].email, 'demo@example.invalid')
})

// a throwing serializer must run exactly once, the input must be restored,
// and the original error object must reach the caller
check('serializer throws once and input is restored', () => {
  const error = new Error('sample failure')
  let calls = 0
  const redact = fastRedact({
    paths: ['token', 'rows.*.email'],
    serialize () {
      calls++
      throw error
    }
  })
  const o = { token: 'PUBLIC_SAMPLE_A', rows: [{ email: 'demo@example.invalid' }] }
  const before = JSON.stringify(o)
  assert.throws(() => redact(o), (e) => e === error)
  assert.equal(calls, 1)
  assert.equal(JSON.stringify(o), before)
})

// consecutive reuse: the same redactor keeps working correctly after a
// failed call and after a successful one
check('consecutive reuse after failure and success', () => {
  const error = new Error('sample failure')
  let fail = true
  const redact = fastRedact({
    paths: ['token', 'rows.*.email'],
    serialize (o) {
      if (fail) throw error
      return JSON.stringify(o)
    }
  })
  const first = { token: 'PUBLIC_SAMPLE_A', rows: [{ email: 'demo@example.invalid' }] }
  assert.throws(() => redact(first), (e) => e === error)
  assert.equal(first.token, 'PUBLIC_SAMPLE_A')
  assert.equal(first.rows[0].email, 'demo@example.invalid')

  fail = false
  const second = { token: 'PUBLIC_SAMPLE_B', rows: [{ email: 'demo@example.invalid' }] }
  assert.equal(redact(second), '{"token":"[REDACTED]","rows":[{"email":"[REDACTED]"}]}')
  assert.equal(second.token, 'PUBLIC_SAMPLE_B')
  assert.equal(second.rows[0].email, 'demo@example.invalid')

  const third = { token: 'PUBLIC_SAMPLE_A', rows: [{ email: 'demo@example.invalid' }] }
  assert.equal(redact(third), '{"token":"[REDACTED]","rows":[{"email":"[REDACTED]"}]}')
  assert.equal(third.token, 'PUBLIC_SAMPLE_A')
})

// nested invocation whose inner call fails and is caught by the outer
// serializer: the outer restore records must survive, the outer event must
// still be redacted when the outer serializer resumes, and it is only
// restored when the outer call ends
check('nested inner failure caught by outer serializer', () => {
  const innerError = new Error('sample failure')
  let inside = false
  let outerTokenDuringSerialize
  const redact = fastRedact({
    paths: ['token'],
    serialize (o) {
      if (!inside) {
        inside = true
        const inner = { token: 'PUBLIC_SAMPLE_B' }
        // the inner call re-enters this same serializer, which now throws
        assert.throws(() => redact(inner), (e) => e === innerError)
        // the failed inner call restored its own input
        assert.equal(inner.token, 'PUBLIC_SAMPLE_B')
        inside = false
      } else {
        throw innerError
      }
      outerTokenDuringSerialize = o.token
      return JSON.stringify(o)
    }
  })
  const outer = { token: 'PUBLIC_SAMPLE_A' }
  assert.equal(redact(outer), '{"token":"[REDACTED]"}')
  // the outer event was still redacted when the outer serializer resumed
  assert.equal(outerTokenDuringSerialize, '[REDACTED]')
  // and only restored once the outer call ended
  assert.equal(outer.token, 'PUBLIC_SAMPLE_A')
})

// a field whose original value is `undefined` must round-trip: redacted
// while serializing, restored to `undefined` afterwards
check('original field value undefined is restored', () => {
  const redact = fastRedact({ paths: ['payload.*'] })
  const o = { payload: { token: undefined, email: 'demo@example.invalid' } }
  assert.equal(redact(o), '{"payload":{"token":"[REDACTED]","email":"[REDACTED]"}}')
  assert.equal(Object.prototype.hasOwnProperty.call(o.payload, 'token'), true)
  assert.equal(o.payload.token, undefined)
  assert.equal(o.payload.email, 'demo@example.invalid')
})

// `remove: true` output is unchanged by the rollback work
check('remove true output preserved', () => {
  const redact = fastRedact({ paths: ['token', 'rows.*.email'], remove: true })
  const o = { token: 'PUBLIC_SAMPLE_A', rows: [{ email: 'demo@example.invalid' }], count: 2 }
  assert.equal(redact(o), '{"rows":[{}],"count":2}')
  assert.equal(o.token, 'PUBLIC_SAMPLE_A')
  assert.equal(o.rows[0].email, 'demo@example.invalid')
})

process.exitCode = failures ? 1 : 0
