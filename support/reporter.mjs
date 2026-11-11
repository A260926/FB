export default async function* reporter(events) {
  const expected = JSON.parse(process.env.AUTHOR_EXPECTED_TESTS);
  const passed = new Map();
  let bad = false;
  for await (const event of events) {
    const data = event.data;
    if (event.type === 'test:stdout' || event.type === 'test:stderr') {
      yield data.message;
    }
    if (event.type === 'test:fail' || (event.type === 'test:pass' && (data.skip || data.todo))) {
      bad = true;
      yield `FAIL_OR_SKIP ${data.name}\n`;
      if (data.details?.error) yield `${data.details.error.stack || data.details.error}\n`;
    }
    if (event.type === 'test:pass' && data.nesting === 0 && !data.skip && !data.todo) {
      passed.set(data.name, (passed.get(data.name) || 0) + 1);
      yield `PASS ${data.name}\n`;
    }
  }
  for (const name of expected) {
    if (passed.get(name) !== 1) {
      bad = true;
      yield `REQUIRED_TEST_NOT_PASSED ${name}\n`;
    }
  }
  if (bad) throw new Error('Required tests failed, skipped, or were not executed exactly once');
  yield `CHECK_OK ${expected.length} required tests\n`;
}
