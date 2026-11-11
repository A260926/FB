const assert = require('node:assert/strict');
const rewire = require('rewire');
process.env.TAIKO_ENABLE_TEST_HOOKS = '1';
const taiko = require('../packages/taiko/lib/taiko');
const {eventHandler, eventRegexMap} = require('../packages/taiko/lib/eventBus');
const page = rewire('../packages/taiko/lib/handlers/pageHandler.js');
let opening;
const wire = [];
const revert = page.__set__('page', {
  javascriptDialogOpening(fn) { opening = fn; },
  async handleJavaScriptDialog(value) { wire.push(value); }
});
taiko.__set__('validate', () => {});
taiko.__set__('pageHandler', page);
page.addJavascriptDialogOpeningListener();
(async () => {
  const calls = [];
  taiko.confirm('Continue', async data => { calls.push('first:'+data.message); await taiko.accept(); });
  taiko.confirm('Continue', async data => { calls.push('second:'+data.message); await taiko.accept(); });
  opening({type:'confirm', message:'Continue', url:'https://local.invalid/form'});
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(calls, ['first:Continue','second:Continue']);
  assert.equal(wire.length, 2);
  assert.ok(wire.every(x => x.accept === true));
  assert.equal(eventRegexMap.size, 0);
  console.log('BASELINE same-message-dialog callbacks=2 accept-commands=2 one-opening=1');
  revert(); taiko.__reset__(); eventRegexMap.clear();
  eventHandler.removeAllListeners('confirm');
})().catch(error => { console.error(error); process.exitCode=1; });
