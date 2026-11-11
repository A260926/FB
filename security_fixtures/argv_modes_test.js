'use strict';

// Regression tests for `parse(s, env, { argvOnly: true })`.
// String-only: nothing here executes the parsed input or touches the network.

const assert = require('node:assert/strict');
const { parse, quote } = require('..');

const argvOnly = { argvOnly: true };

// --- basic tokenization -----------------------------------------------------

assert.deepEqual(parse('tool "a b" \'c;d\' \\*', {}, argvOnly), ['tool', 'a b', 'c;d', '*']);
assert.deepEqual(parse('  spaced   out\targs  ', {}, argvOnly), ['spaced', 'out', 'args']);
assert.deepEqual(parse('', {}, argvOnly), []);
assert.deepEqual(parse('   \t  ', {}, argvOnly), []);

// adjacent quoted fragments concatenate; empty quotes stay an empty argument
assert.deepEqual(parse('a\'b\'"c"d', {}, argvOnly), ['abcd']);
assert.deepEqual(parse('tool \'\' ""', {}, argvOnly), ['tool', '', '']);
assert.deepEqual(parse('\'\'x""', {}, argvOnly), ['x']);

// --- quote/escape counterexamples: swapping quoting for escaping ------------

// the same literal `*` arrived at through three different surfaces
assert.deepEqual(parse('\\*', {}, argvOnly), ['*']);
assert.deepEqual(parse('\'*\'', {}, argvOnly), ['*']);
assert.deepEqual(parse('"*"', {}, argvOnly), ['*']);
// and the bare character is rejected, so the three above are not equivalent to it
assert.throws(() => parse('*', {}, argvOnly), SyntaxError);
assert.throws(() => parse('a?b', {}, argvOnly), SyntaxError);

// escaped control operators are data; bare ones are rejected
assert.deepEqual(parse('\\; \\| \\& \\( \\) \\< \\> \\#', {}, argvOnly), [';', '|', '&', '(', ')', '<', '>', '#']);
for (const op of [';', '|', '&', '(', ')', '<', '>', '#']) {
  assert.throws(() => parse(`tool ${op} rest`, {}, argvOnly), SyntaxError, `bare ${op}`);
}

// backslash escapes differ per quote context
assert.deepEqual(parse('"\\n"', {}, argvOnly), ['\\n']); // not one of \\ " $ ` inside double quotes: kept literally
assert.deepEqual(parse('"\\\\ \\" \\$ \\`"', {}, argvOnly), ['\\ " $ `']);
assert.deepEqual(parse('\'\\n\'', {}, argvOnly), ['\\n']); // single quotes: backslash is literal
assert.deepEqual(parse('\\n', {}, argvOnly), ['n']); // unquoted: backslash escapes the next character
assert.throws(() => parse('dangling \\', {}, argvOnly), SyntaxError);
assert.throws(() => parse('continued \\\nline', {}, argvOnly), SyntaxError); // no line continuation
assert.deepEqual(parse('"a\\\nb"', {}, argvOnly), ['a\\\nb']); // double quotes: newline is not escapable, so the backslash stays literal

// newlines: data inside quotes, rejected bare
assert.deepEqual(parse('\'a\nb\'', {}, argvOnly), ['a\nb']);
assert.deepEqual(parse('"a\nb"', {}, argvOnly), ['a\nb']);
assert.throws(() => parse('a\nb', {}, argvOnly), SyntaxError);
assert.throws(() => parse('a\rb', {}, argvOnly), SyntaxError);
assert.throws(() => parse('a\0b', {}, argvOnly), SyntaxError);
assert.throws(() => parse('\'a\0b\'', {}, argvOnly), SyntaxError);

// unterminated quotes
assert.throws(() => parse('\'unterminated', {}, argvOnly), SyntaxError);
assert.throws(() => parse('"unterminated', {}, argvOnly), SyntaxError);

// --- command substitution / unsupported expansions --------------------------

assert.throws(() => parse('`id`', {}, argvOnly), SyntaxError);
assert.throws(() => parse('"`id`"', {}, argvOnly), SyntaxError);
assert.deepEqual(parse('\'`id`\'', {}, argvOnly), ['`id`']); // single quotes keep it literal
assert.throws(() => parse('$(id)', {}, argvOnly), SyntaxError);
assert.throws(() => parse('"$(id)"', {}, argvOnly), SyntaxError);
assert.deepEqual(parse('\'$(id)\'', {}, argvOnly), ['$(id)']);
assert.throws(() => parse('$\'x\'', {}, argvOnly), SyntaxError); // ANSI-C quoting unsupported
assert.throws(() => parse('${X:-y}', {}, argvOnly), SyntaxError);
assert.throws(() => parse('${#X}', {}, argvOnly), SyntaxError);
assert.throws(() => parse('${}', {}, argvOnly), SyntaxError);
assert.throws(() => parse('${X', {}, argvOnly), SyntaxError);
assert.throws(() => parse('$1 $* $@ $$ $? $!', {}, argvOnly), SyntaxError);
assert.throws(() => parse('lone $', {}, argvOnly), SyntaxError);
assert.throws(() => parse('"$"', {}, argvOnly), SyntaxError);

// --- environment expansion ---------------------------------------------------

assert.deepEqual(parse('a $B "${C}d" e', { B: 'x', C: 'y' }, argvOnly), ['a', 'x', 'yd', 'e']);
assert.deepEqual(parse('$MISSING ${MISSING}', {}, argvOnly), ['', '']); // undefined -> empty string
assert.deepEqual(parse('a$MISSING"b"', {}, argvOnly), ['ab']);
assert.deepEqual(parse('$V', { V: '  spaced  out  ' }, argvOnly), ['  spaced  out  ']); // no field splitting
assert.deepEqual(parse('$V', undefined, argvOnly), ['']); // no env at all
assert.deepEqual(parse('$V', null, argvOnly), ['']);

// source/value swap counterexample: the source rejects these characters,
// but the very same characters arriving through an expansion are kept whole
const hostile = '; $(literal) * # x\n`y` | & < >';
assert.throws(() => parse(`tool ${hostile}`, {}, argvOnly), SyntaxError);
assert.deepEqual(parse('tool $VALUE', { VALUE: hostile }, argvOnly), ['tool', hostile]);
assert.deepEqual(parse('tool "$VALUE"', { VALUE: hostile }, argvOnly), ['tool', hostile]);
// and the reverse: a value that is valid source syntax is still not re-parsed
assert.deepEqual(parse('$V', { V: '\'quoted\' \\$X' }, argvOnly), ['\'quoted\' \\$X']);

// non-string env values are rejected, including operator-shaped objects
for (const bad of [{ op: ';' }, { op: 'glob', pattern: '*' }, null, 42, ['x'], true]) {
  assert.throws(() => parse('tool $V', { V: bad }, argvOnly), TypeError, JSON.stringify(bad));
  assert.throws(() => parse('tool $V', () => bad, argvOnly), TypeError, JSON.stringify(bad));
}

// --- env is consulted only after the whole source validates ------------------

{
  let calls = 0;
  const env = () => { calls += 1; return 'data'; };
  assert.throws(() => parse('tool $VALUE "unterminated', env, argvOnly), SyntaxError);
  assert.equal(calls, 0, 'resolver must not run for a malformed source');
}

{
  // property getters count as lookups and must also be deferred
  let gets = 0;
  const env = { get VALUE() { gets += 1; return 'data'; } };
  assert.throws(() => parse('tool $VALUE trailing\\', env, argvOnly), SyntaxError);
  assert.equal(gets, 0, 'getter must not run for a malformed source');
  assert.deepEqual(parse('tool $VALUE', env, argvOnly), ['tool', 'data']);
  assert.equal(gets, 1);
}

// trailing illegal fragments after a valid prefix: rejected before any lookup
{
  let calls = 0;
  const env = () => { calls += 1; return 'data'; };
  const badTails = ['tool $OK ;', 'tool $OK | other', 'tool $OK *', 'tool $OK # c', 'tool $OK `x`', 'tool $OK $(x)', 'tool $OK\n', 'tool $OK \\'];
  for (const source of badTails) {
    assert.throws(() => parse(source, env, argvOnly), SyntaxError, source);
  }
  assert.equal(calls, 0, 'no lookups for any malformed source');
}

// invalid option combinations throw TypeError before touching env
{
  let calls = 0;
  const env = () => { calls += 1; return 'data'; };
  assert.throws(() => parse('tool $V', env, { argvOnly: true, splitUnquoted: true }), TypeError);
  assert.throws(() => parse('tool $V', env, { argvOnly: true, splitUnquoted: ':' }), TypeError);
  assert.throws(() => parse('tool $V', env, { argvOnly: true, escape: '^' }), TypeError);
  assert.equal(calls, 0);
  // benign combinations still work
  assert.deepEqual(parse('tool $V', { V: 'x' }, { argvOnly: true, splitUnquoted: false }), ['tool', 'x']);
  assert.deepEqual(parse('tool $V', { V: 'x' }, { argvOnly: true, escape: '\\' }), ['tool', 'x']);
}

// env and opts are not mutated
{
  const env = { V: 'x' };
  const opts = { argvOnly: true };
  parse('tool $V', env, opts);
  assert.deepEqual(env, { V: 'x' });
  assert.deepEqual(opts, { argvOnly: true });
}

// --- quote() round-trip of pure string arguments -----------------------------

{
  const args = ['tool', '*', 'a;b', 'x y', '', 'it\'s', 'say "hi"', '$HOME', '`tick`', '#hash', 'line\nbreak', 'back\\slash', 'tab\there'];
  assert.deepEqual(parse(quote(args), {}, argvOnly), args);
}

// --- default mode compatibility ----------------------------------------------

assert.deepEqual(parse('tool \\*'), ['tool', { op: 'glob', pattern: '*' }]);
assert.deepEqual(parse('beep || boop > /byte'), ['beep', { op: '||' }, 'boop', { op: '>' }, '/byte']);
assert.deepEqual(parse('a # c'), ['a', { comment: ' c' }]);
assert.deepEqual(parse('a $T', { T: 'c d' }, { splitUnquoted: true }), ['a', 'c', 'd']);
assert.deepEqual(parse('beep ^--boop="$PWD"', { PWD: '/home/robot' }, { escape: '^' }), ['beep', '--boop=/home/robot']);
assert.deepEqual(parse('a $X', () => ({ op: ';' })), ['a', { op: ';' }]); // object callback still flows through
assert.deepEqual(parse('echo $\'it\\\'s\'', {}, {}), ['echo', "it's"]); // ANSI-C quoting still on by default

console.log('argv_modes_test OK');
