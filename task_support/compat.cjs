const assert = require('node:assert/strict')
const Xray = require('..')
let callbackSeen = false
Xray({ filters: { upper: v => v.toUpperCase() } })('<h1>old</h1>', { name: 'h1 | upper' })((err, data) => {
  assert.ifError(err); assert.deepEqual(data, { name: 'OLD' }); callbackSeen = true
})
assert.equal(callbackSeen, false)
Promise.resolve().then(() => assert.equal(callbackSeen, true))
Xray()('<ul><li><b>A</b></li><li><b>B</b></li></ul>', 'li', [{ name: 'b' }])((err, data) => {
 assert.ifError(err); assert.deepEqual(data, [{name:'A'},{name:'B'}]); console.log('SYNC_EXTRACTION_COMPAT=ok')
})
