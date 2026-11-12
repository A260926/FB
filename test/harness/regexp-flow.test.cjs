'use strict';

// Flow-sensitive RegExp constructor checks. Every sample program below is
// only parsed and linted; the dangerous patterns and any external calls in
// the samples are never executed. Expected messages, counts, and locations
// are written out literally and do not depend on the production analyzer.

const { test } = require('node:test');
const assert = require('node:assert/strict');
const { Linter } = require('eslint');
const security = require('../../index.js');

const UNSAFE_RULE = 'security/detect-unsafe-regex';
const NON_LITERAL_RULE = 'security/detect-non-literal-regexp';
const UNSAFE_MESSAGE = 'Unsafe Regular Expression (new RegExp)';
const UNSAFE_LITERAL_MESSAGE = 'Unsafe Regular Expression';
const NON_LITERAL_MESSAGE = 'Found non-literal argument to RegExp Constructor';

const BOTH_RULES = { [UNSAFE_RULE]: 'error', [NON_LITERAL_RULE]: 'error' };

/**
 * Lints a sample program and returns its messages in a deterministic order.
 * @param {string} code The sample program (only statically read).
 * @param {Object} rules The rule configuration to use.
 * @param {Linter} [linter] The linter instance to use.
 * @returns {Array<{ruleId: string, message: string, line: number, column: number}>}
 */
function lint(code, rules, linter = new Linter()) {
  const messages = linter.verify(code, {
    plugins: { security },
    languageOptions: { ecmaVersion: 2022, sourceType: 'module' },
    rules,
  });
  assert.ok(!messages.some((message) => message.fatal), `sample must parse: ${code}`);
  return messages
    .map((message) => ({ ruleId: message.ruleId, message: message.message, line: message.line, column: message.column }))
    .sort((a, b) => a.line - b.line || a.column - b.column || a.ruleId.localeCompare(b.ruleId) || a.message.localeCompare(b.message));
}

test('program-points', () => {
  // A dangerous value is reported where it is live, and disappears after an
  // overwrite with a safe value.
  assert.deepStrictEqual(lint(['let p = "(a+)+$";', 'RegExp(p);', 'p = "^a+$";', 'RegExp(p);'].join('\n'), BOTH_RULES), [
    { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 2, column: 1 },
  ]);

  // Copying a string to another variable keeps the old value: later writes
  // to the source do not change the copy.
  assert.deepStrictEqual(lint(['let a = "(a+)+$";', 'let b = a;', 'a = "^a+$";', 'RegExp(b);', 'RegExp(a);'].join('\n'), BOTH_RULES), [
    { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 4, column: 1 },
  ]);

  // String concatenation and templates produce known patterns.
  assert.deepStrictEqual(lint(['const head = "(a+)";', 'const p = `${head}+$`;', 'RegExp(p);', 'RegExp("^" + "a" + "+$");'].join('\n'), BOTH_RULES), [
    { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 3, column: 1 },
  ]);

  // Uninitialized variables start unknown and recover after a definite
  // assignment.
  assert.deepStrictEqual(lint(['let p;', 'RegExp(p);', 'p = "^a+$";', 'RegExp(p);'].join('\n'), BOTH_RULES), [
    { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 2, column: 1 },
  ]);

  // Parameters start unknown and recover after a definite assignment.
  assert.deepStrictEqual(lint(['function f(p) {', '  RegExp(p);', '  p = "^a+$";', '  RegExp(p);', '}'].join('\n'), BOTH_RULES), [
    { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 2, column: 3 },
  ]);

  // Recognized static expressions that cannot be evaluated locally (path
  // construction, import.meta) and read-only bindings of them stay quiet.
  assert.deepStrictEqual(
    lint(['import path from "node:path";', 'const joined = path.join("a", "b");', 'RegExp(joined);', 'RegExp(path.join("a", "b"));', 'RegExp(import.meta.url);'].join('\n'), BOTH_RULES),
    []
  );

  // A parameter or local binding named RegExp is not the built-in
  // constructor, so neither rule reports it.
  assert.deepStrictEqual(
    lint(['function f(RegExp) {', '  RegExp("(a+)+$");', '}', 'function g() {', '  const RegExp = String;', '  RegExp("(a+)+$");', '}'].join('\n'), BOTH_RULES),
    []
  );

  // Literal forms keep their existing messages and locations.
  assert.deepStrictEqual(lint(['RegExp("(a+)+$");', 'new RegExp("(a+)+$");', 'const r = /(a+)+$/;'].join('\n'), BOTH_RULES), [
    { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 1, column: 1 },
    { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 2, column: 1 },
    { ruleId: UNSAFE_RULE, message: UNSAFE_LITERAL_MESSAGE, line: 3, column: 11 },
  ]);

  // A dynamic write is not hidden by the static initial declaration.
  assert.deepStrictEqual(lint(['let p = "^a+$";', 'p = input;', 'RegExp(p);'].join('\n'), BOTH_RULES), [
    { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 3, column: 1 },
  ]);

  // Same-named bindings in different blocks are distinct lexical variables:
  // the inner safe value does not touch the outer dangerous one.
  assert.deepStrictEqual(lint(['let p = "(a+)+$";', '{', '  let p = "^a+$";', '  RegExp(p);', '}', 'RegExp(p);'].join('\n'), BOTH_RULES), [
    { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 6, column: 1 },
  ]);
});

test('joins-and-exits', () => {
  // Both branches assign a constant: the join keeps both candidates, and the
  // dangerous one is reported without a non-literal warning.
  assert.deepStrictEqual(
    lint(['function f(flag) {', '  let p;', '  if (flag) {', '    p = "(a+)+$";', '  } else {', '    p = "^a+$";', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 8, column: 3 }]
  );

  // One branch assigns a dangerous constant, the other an external input:
  // both rules report once each at the shared call site.
  assert.deepStrictEqual(
    lint(
      ['function f(flag, input) {', '  let p;', '  if (flag) {', '    p = "(a+)+$";', '  } else {', '    p = input;', '  }', '  RegExp(p);', '}'].join('\n'),
      BOTH_RULES
    ),
    [
      { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 8, column: 3 },
      { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 8, column: 3 },
    ]
  );

  // A dangerous assignment on a branch that already returned does not
  // pollute the safe call after it.
  assert.deepStrictEqual(lint(['function f(flag) {', '  let p = "^a+$";', '  if (flag) {', '    p = "(a+)+$";', '    return;', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES), []);

  // A thrown-away branch does not pollute the call after it either.
  assert.deepStrictEqual(
    lint(['function f(flag) {', '  let p = "^a+$";', '  if (flag) {', '    p = "(a+)+$";', '    throw new Error("x");', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    []
  );

  // Switch fallthrough and the post-switch join both see the dangerous
  // candidate.
  assert.deepStrictEqual(
    lint(
      [
        'function f(x) {',
        '  let p = "^a+$";',
        '  switch (x) {',
        '    case 1:',
        '      p = "(a+)+$";',
        '    case 2:',
        '      RegExp(p);',
        '      break;',
        '    default:',
        '      p = "z";',
        '  }',
        '  RegExp(p);',
        '}',
      ].join('\n'),
      BOTH_RULES
    ),
    [
      { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 7, column: 7 },
      { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 12, column: 3 },
    ]
  );

  // A break carries the loop body's writes to the loop exit.
  assert.deepStrictEqual(
    lint(['function f(flag) {', '  let p = "^a+$";', '  while (flag) {', '    p = "(a+)+$";', '    break;', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 7, column: 3 }]
  );

  // A write inside a try block may not complete, so the previous dangerous
  // candidate survives the join conservatively.
  assert.deepStrictEqual(
    lint(['function f() {', '  let p = "(a+)+$";', '  try {', '    p = "^a+$";', '  } catch (e) {', '    p = "z";', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 8, column: 3 }]
  );
});

test('loop-fixpoint', () => {
  // The call inside the loop sees the previous iteration's write through the
  // back edge; reporting only the first traversal would miss this.
  assert.deepStrictEqual(
    lint(['function f(flag) {', '  let p = "^a+$";', '  while (flag) {', '    RegExp(p);', '    p = "(a+)+$";', '  }', '}'].join('\n'), BOTH_RULES),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 4, column: 5 }]
  );

  // A while loop may run zero times, so the pre-loop dangerous value is
  // still live after it...
  // ...but a do-while body runs at least once, overwriting it for sure.
  assert.deepStrictEqual(
    lint(
      [
        'function f(flag) {',
        '  let p = "(a+)+$";',
        '  while (flag) {',
        '    p = "^a+$";',
        '  }',
        '  RegExp(p);',
        '}',
        'function g(flag) {',
        '  let p = "(a+)+$";',
        '  do {',
        '    p = "^a+$";',
        '  } while (flag);',
        '  RegExp(p);',
        '}',
      ].join('\n'),
      BOTH_RULES
    ),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 6, column: 3 }]
  );

  // Continue carries the state at the end of the iteration back to the loop
  // head.
  assert.deepStrictEqual(
    lint(
      ['function f(flag) {', '  let p = "^a+$";', '  while (flag) {', '    if (flag) {', '      p = "(a+)+$";', '      continue;', '    }', '    RegExp(p);', '  }', '}'].join('\n'),
      BOTH_RULES
    ),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 8, column: 5 }]
  );

  // A for loop's back edge flows through the update.
  assert.deepStrictEqual(
    lint(['function f(flag) {', '  let p = "^a+$";', '  for (let i = 0; i < 3; i++) {', '    p = "(a+)+$";', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 6, column: 3 }]
  );

  // An unknown value alone never produces an unsafe report.
  assert.deepStrictEqual(
    lint(['function f(flag, input) {', '  let p = input;', '  while (flag) {', '    p = "^a+$";', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    [{ ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 6, column: 3 }]
  );

  // Growing a string in a loop exceeds the candidate budget and degrades to
  // unknown (reported as non-literal); a later definite overwrite recovers a
  // known dangerous value.
  assert.deepStrictEqual(
    lint(['function f(flag) {', '  let p = "^a+$";', '  while (flag) {', '    p = p + "a";', '    RegExp(p);', '  }', '  p = "(a+)+$";', '  RegExp(p);', '}'].join('\n'), BOTH_RULES),
    [
      { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 5, column: 5 },
      { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 8, column: 3 },
    ]
  );

  // A single string longer than 4096 UTF-16 code units also degrades to
  // unknown, while exactly 4096 code units are still tracked.
  const tooLong = 'a'.repeat(4097);
  assert.deepStrictEqual(lint(`let p = "${tooLong}";\nRegExp(p);`, BOTH_RULES), [{ ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 2, column: 1 }]);
  const exactlyLongEnough = 'a'.repeat(4096);
  assert.deepStrictEqual(lint(`let p = "${exactlyLongEnough}";\nRegExp(p);`, BOTH_RULES), []);

  // Concatenation inside a loop can also exceed the length budget.
  assert.deepStrictEqual(lint(['function f(flag) {', '  let p = "a";', '  while (flag) {', '    p = p + p;', '  }', '  RegExp(p);', '}'].join('\n'), BOTH_RULES), [
    { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 6, column: 3 },
  ]);
});

test('rule-isolation', () => {
  const sample = ['function f(flag, input) {', '  let p;', '  if (flag) {', '    p = "(a+)+$";', '  } else {', '    p = input;', '  }', '  RegExp(p);', '}'].join('\n');
  const expectedUnsafe = { ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 8, column: 3 };
  const expectedNonLiteral = { ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 8, column: 3 };

  // Enabling only one rule reports exactly that rule's share of the
  // both-rules result.
  assert.deepStrictEqual(lint(sample, BOTH_RULES), [expectedNonLiteral, expectedUnsafe]);
  assert.deepStrictEqual(lint(sample, { [UNSAFE_RULE]: 'error' }), [expectedUnsafe]);
  assert.deepStrictEqual(lint(sample, { [NON_LITERAL_RULE]: 'error' }), [expectedNonLiteral]);

  // A shadowed RegExp is ignored no matter which rule is enabled.
  const shadowed = 'function f(RegExp) {\n  RegExp("(a+)+$");\n}';
  assert.deepStrictEqual(lint(shadowed, { [UNSAFE_RULE]: 'error' }), []);
  assert.deepStrictEqual(lint(shadowed, { [NON_LITERAL_RULE]: 'error' }), []);

  // Reusing one Linter across files and swapped rule configurations does not
  // leak analysis state between runs.
  const linter = new Linter();
  const dangerousFile = 'let p = "(a+)+$";\nRegExp(p);';
  const dynamicFile = 'let p = input;\nRegExp(p);';
  const cleanFile = 'RegExp("^a+$");';
  const expectedDangerous = [{ ruleId: UNSAFE_RULE, message: UNSAFE_MESSAGE, line: 2, column: 1 }];
  const expectedDynamic = [{ ruleId: NON_LITERAL_RULE, message: NON_LITERAL_MESSAGE, line: 2, column: 1 }];

  assert.deepStrictEqual(lint(dangerousFile, BOTH_RULES, linter), expectedDangerous);
  assert.deepStrictEqual(lint(dynamicFile, BOTH_RULES, linter), expectedDynamic);
  assert.deepStrictEqual(lint(cleanFile, BOTH_RULES, linter), []);
  assert.deepStrictEqual(lint(dangerousFile, { [NON_LITERAL_RULE]: 'error' }, linter), []);
  assert.deepStrictEqual(lint(dynamicFile, { [UNSAFE_RULE]: 'error' }, linter), []);
  assert.deepStrictEqual(lint(dangerousFile, BOTH_RULES, linter), expectedDangerous);
  assert.deepStrictEqual(lint(dynamicFile, BOTH_RULES, linter), expectedDynamic);
});
