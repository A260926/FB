# Stream acquisition contract

This regression suite pins the relationship between three things in the
streaming acquisition path:

1. **Decoding context** — the same reference text means different things in
   element text, in an attribute value, inside a raw-text element, and in XML
   mode.
2. **UTF-16 source ranges** — `startIndex`/`endIndex` on parser callbacks and
   on the DOM nodes produced by `parseDocument` and `createDocumentStream`
   must describe the original input, not the decoded output.
3. **Pause/`end` handoff** — text and open-tag callbacks can pause the parser;
   chunks and `end` written while paused must be delivered exactly once after
   an external `resume`, and `onend` must fire exactly once.

The production sources in `src/` are not modified. Everything is implemented
with Node's built-in test runner and JavaScript; there are no Python, browser,
or extra dependencies.

## Files

| File | Purpose |
| --- | --- |
| `test/stream-contract.mjs` | The test suite. Its single top-level test is named `stream acquisition contract`. |
| `test/run-stream-contract.mjs` | Builds the library, runs the normal suite, then rebuilds three degraded source trees and verifies the suite catches each one. |

`harness/install.sh` installs the dependencies; `harness/check-target.sh`
performs the full acceptance run (TAP capture, report validation, degraded
build checks, and the static baseline).

## Generated pages

The pages (15 of them) are not hand-written HTML strings. A small model
builder assembles each document from three inputs:

- elements with a name and a child list,
- text runs made of literal parts and reference parts,
- attributes with a quoting style and a value made of literal and reference
  parts.

While emitting, the builder records, for every page:

- the exact HTML source,
- the inclusive UTF-16 interval of every text fragment and reference,
- the expected callback values,
- the expected attribute source slices and inclusive `endIndex` values,
- the expected element/text/comment tree with `startIndex`/`endIndex`.

The scope is deliberately small: element names and nested balanced tags,
single- and double-quoted attributes, `title`/`textarea` RCDATA, `script`
and `style` raw text, comments, and HTML and XML modes. There is no attempt
to model a full browser parser.

### Reference vocabulary

Expected values come from hand-written finite maps in the test file, never
from `entities`, `parseDocument`, or the parser under test:

- `&amp;`, `&lt;`, `&gt;`, `&quot;`, `&apos;`, `&nbsp;`, `&not;`,
- decimal references such as `&#65;` and `&#128512;`,
- hexadecimal references such as `&#x41;`, `&#X41;`, and `&#x1F600;`,
- one named reference that decodes to two code points, `&NotEqualTilde;`
  (`≂` + `◌̸`).

Two reference classes are kept distinct:

- **semicolonless text references**: without a semicolon, `&amp` and `&not`
  decode in ordinary text (legacy behavior);
- **attribute-blocked references**: inside an attribute value, a
  semicolonless reference followed by an ASCII letter, digit, or `=` stays
  literal (`&notit=7`, `&ampx`, `&amp1`, `&amp=`).

The suite also covers `decodeEntities: false`, where no reference decodes
anywhere. `&amp;` inside `script` stays literal (`&amp;`), while the same
reference inside `textarea` decodes to `&`.

## What is compared

For every page, the suite first collects events through the public `Parser`
callbacks. For every schedule it compares:

- attribute events: name, decoded value, quote style, and the source slice
  from `startIndex` through the inclusive `endIndex`. Attribute positions are
  checked against the complete attribute source fragment; decoded value
  lengths are never used to infer positions.
- opening tags: name, attribute object, implied flag, and the whole tag range.
- closing tags: name, implied flag, and range.
- comments: content and range.
- text: consecutive text callbacks are merged (their number varies with the
  chunking); values must concatenate to the expected text in the original
  order. Ranges are compared as the union of non-empty intervals for the same
  text run. A single callback that crosses a reference boundary is rejected.
  A multi-code-point reference may produce an extra zero-width follow-up
  callback; the contract does not require one interval per code point.
- `onend`: exactly once per document.

Each page is parsed with all of these chunk schedules:

- one unsplit input,
- every single split point (all `length - 1` positions),
- one UTF-16 unit (`charAt`) per write,
- six fixed-seed multi-cut schedules (seeds `1`–`6`, four cuts each).

Those schedules routinely place a cut inside a reference, between the
surrogate halves of a non-BMP character, between `</` and the tag name, and
between the two quote characters of an attribute.

Then, with independent expectations, the same page is checked through:

- `parseDocument` with `withStartIndices` and `withEndIndices`,
- `createDocumentStream` fed one UTF-16 unit at a time.

Both trees must match the model's element names, attribute values, text and
comment contents, and node `startIndex`/`endIndex` values. `parseDocument` is
not treated as the sole source of truth; the callback observations and the
model's recorded values are compared independently.

## Pause scheduling

The pause path is deterministic. For a selected text callback or open-tag
callback, the test:

1. pauses inside that callback,
2. synchronously enqueues the remaining chunks and `end`,
3. resumes from outside the parser call stack (a later macrotask), and
4. checks the full merged text, attribute values, tag sequence, comments,
   absence of parser errors, and exactly one `onend`.

The group of callbacks for the token already in progress is allowed to
finish; JavaScript stacks are never forcibly interrupted. Separate subtests
cover a pause taken before the first write and a pause followed by a queued
`end` with no second `end`, and the reset path: after `reset`, a second model
with different IDs and text must start its ranges at zero.

## Degradation checks

After the normal suite passes, `test/run-stream-contract.mjs` copies `src`
into a temporary directory, applies one surgical change, compiles that copy
with the repository's `tsc`, imports its `dist/index.js`, and runs the same
test file against it. A degradation only counts as caught when the run
compiles, loads, executes a non-zero number of tests, and fails with a value,
position, or count assertion — not with a compile error, import error, crash,
or zero tests.

| Degradation | Change | Signal |
| --- | --- | --- |
| `STREAM_ATTRIBUTE_CAUGHT` | attribute values decode in legacy text mode instead of attribute mode | attribute values change |
| `STREAM_RANGE_CAUGHT` | the entity text callback's `endIndex` is one unit too large | text ranges and tree indices change |
| `STREAM_END_CAUGHT` | `resume` no longer delivers an `end` queued during a pause | `onend` count drops to zero |

The runner writes one `STREAM_MUTATION` line per check with the patched file,
anchor hit count, number of failing assertions, and sample failing test names.
The normal build is not modified: each check compiles a fresh copy and the
temporary directory is removed afterwards.

## Running

```bash
bash harness/install.sh
bash harness/check-target.sh
```

The check entry point writes the normal TAP output to
`.build/stream-normal.tap`, validates the summary, then runs the degradation
checks and the static baseline. Expected successful markers:

```text
STREAM_MODEL_OK
STREAM_ATTRIBUTE_CAUGHT
STREAM_RANGE_CAUGHT
STREAM_END_CAUGHT
```

To reproduce a single failure, run the main test directly:

```bash
node --test --test-reporter=tap \
  --test-name-pattern='^stream acquisition contract$' \
  test/stream-contract.mjs
```

To reproduce only the degradation checks after a normal build:

```bash
node test/run-stream-contract.mjs
```
