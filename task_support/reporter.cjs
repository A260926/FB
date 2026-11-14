module.exports = async function * (events) {
  const expected = new Set(['thin-slabs', 'mixed-scale', 'affine-reuse'])
  const passed = new Set()
  let failed = false
  for await (const event of events) {
    const data = event.data || {}
    if (event.type === 'test:diagnostic') yield `DIAGNOSTIC ${data.message}\n`
    if (event.type === 'test:fail') {
      failed = true
      yield `FAIL ${data.name}: ${data.details?.error?.stack || data.details?.error || ''}\n`
    }
    if (event.type === 'test:pass' && (data.skip || data.todo)) failed = true
    if (event.type === 'test:pass' && data.nesting === 0 && expected.has(data.name)) {
      if (data.skip || data.todo || passed.has(data.name)) failed = true
      else {
        passed.add(data.name)
        yield `GROUP ${data.name} COMPLETE\n`
      }
    }
  }
  if (failed || passed.size !== expected.size) throw new Error(`Required tests incomplete: ${[...passed].join(',')}`)
  yield `TARGET_COMPLETE groups=${passed.size}\n`
}
