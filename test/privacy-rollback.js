const assert = require('node:assert/strict')
const fastRedact = require('..')
let failures = 0
function check (name, run) { try { run(); console.log(name + ': PASS') } catch (e) { failures++; console.log(name + ': FAIL ' + e.message) } }
check('normal', () => {
  const o = { token: 'PUBLIC_SAMPLE_A', count: 2 }; const r = fastRedact({ paths: ['token'] })
  assert.equal(r(o), '{"token":"[REDACTED]","count":2}'); assert.equal(o.token, 'PUBLIC_SAMPLE_A')
})
check('serializer rollback', () => {
  const o = { token: 'PUBLIC_SAMPLE_A', rows: [{ email: 'demo@example.invalid' }] }; const before = JSON.stringify(o); const error = new Error('sample failure')
  const r = fastRedact({ paths: ['token', 'rows.*.email'], serialize () { throw error } })
  assert.throws(() => r(o), e => e === error); assert.equal(JSON.stringify(o), before)
})
check('nested serializer', () => {
  const a = { token: 'PUBLIC_SAMPLE_A' }; const b = { token: 'PUBLIC_SAMPLE_B' }; let inside = false
  const r = fastRedact({ paths: ['token'], serialize (o) { if (!inside) { inside = true; assert.equal(r(b), '{"token":"[REDACTED]"}'); inside = false } return JSON.stringify(o) } })
  assert.equal(r(a), '{"token":"[REDACTED]"}'); assert.equal(a.token, 'PUBLIC_SAMPLE_A'); assert.equal(b.token, 'PUBLIC_SAMPLE_B')
})
process.exitCode = failures ? 1 : 0
