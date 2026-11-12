'use strict';
// Records the starting detector behavior. These programs are parsed, never run.
const assert = require('node:assert/strict');
const { Linter } = require('eslint');
const security = require('../index.js');
const cases = [
  ['literal', 'RegExp("(a+)+$");', [1, 0]],
  ['safe', 'RegExp("^a+$");', [0, 0]],
  ['overwrite', 'let p="(a+)+$";\nRegExp(p);\np="^a+$";\nRegExp(p);', [0, 2]],
  ['join', 'function f(flag){let p;if(flag){p="(a+)+$";}else{p="^a+$";}\nRegExp(p);}', [0, 1]],
  ['mixed', 'function f(flag,input){let p;if(flag){p="(a+)+$";}else{p=input;}\nRegExp(p);}', [0, 1]],
  ['backedge', 'function f(flag){let p="^a+$";while(flag){\nRegExp(p);p="(a+)+$";}}', [0, 1]],
  ['early-return', 'function f(flag){let p="^a+$";if(flag){p="(a+)+$";return;}\nRegExp(p);}', [0, 1]],
  ['shadow', 'function f(RegExp){RegExp("(a+)+$");}', [1, 0]],
  ['static-path', 'import path from "node:path";RegExp(path.join("a", "b"));', [0, 0]],
];
for (const [name, code, expected] of cases) {
  const messages = new Linter().verify(code, {
    plugins: { security },
    languageOptions: { ecmaVersion: 2022, sourceType: 'module' },
    rules: {
      'security/detect-unsafe-regex': 'error',
      'security/detect-non-literal-regexp': 'error',
    },
  });
  assert.ok(!messages.some(m => m.fatal), `${name}: parse failed`);
  const counts = ['security/detect-unsafe-regex', 'security/detect-non-literal-regexp']
    .map(id => messages.filter(m => m.ruleId === id).length);
  assert.deepEqual(counts, expected, name);
  console.log(`START ${name} unsafe=${counts[0]} dynamic=${counts[1]} locations=${messages.map(m => `${m.ruleId.split('/')[1]}@${m.line}:${m.column}`).join(',') || '-'}`);
}
console.log('BASELINE_FLOW 9 cases matched upstream behavior');
