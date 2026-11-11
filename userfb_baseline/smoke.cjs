const assert = require('node:assert/strict');
const { Linter } = require('eslint');
const plugin = require('../index.js');
const linter = new Linter();
const config = [{plugins:{'no-unsanitized':plugin},languageOptions:{ecmaVersion:2022},rules:{'no-unsanitized/property':'error','no-unsanitized/method':'error'}}];
const samples = [
 ['const x="hello"; a.innerHTML=x; a.insertAdjacentHTML("beforeend",x);',0],
 ['let x="hello"; x=input; a.innerHTML=x; a.insertAdjacentHTML("beforeend",x);',2],
 ['const x=escapeHTML`hello${input}`; a.innerHTML=x;',0],
 ['let a=b; let b=a; el.innerHTML=a;',1],
];
for (const [source,expected] of samples) {
 const messages = linter.verify(source,config);
 assert.equal(messages.filter(m=>m.fatal).length,0);
 assert.equal(messages.length,expected,JSON.stringify(messages));
}
console.log('BASELINE_DOMXSS safe=0 writes=2 escaped=0 cycle=1');
