# CTU parameter index analysis

Cppcheck can report `ctuArrayIndex` when a function unconditionally accesses a
pointer parameter at an index computed from an integer parameter:

```c
void leaf(int *p, int i) { p[i] = 1; }          // sink
void relay(int n, int *q) { leaf(q, n + 1); }   // forwarding layer
void entry(void) { int a[4] = {0}; relay(3, a); }
```

This document describes how the buffer size and the integer value stay
correlated to the same call, and where the analysis gives up conservatively.

## What is recorded (summaries)

All facts are plain data in the CTU file information; no `Token` pointers or
process global tables are needed after extraction.

* Sink side (`CheckBufferOverrun::getFileInfo`, element `<unsafe-array-index>`):
  for each function that unconditionally reads or writes `p[i]`, `p[i+k]`,
  `p[k+i]` or `p[i-k]` (known integer constant `k`), the summary stores the
  function id, the 1-based numbers of the pointer and the integer parameter,
  the constant offset `k` in elements, the element size in bytes for the
  selected platform, and the access location.
* Call sites (`CTU::getFileInfo`):
  * `<function-call>` with `call-argvalue-known="1"`: an argument is a known
    integer value (recorded in addition to the pre-existing zero/buffer-size
    records, which keep their exact old meaning).
  * `<function-call>` with a buffer size value: an argument is a one
    dimensional array with a compile-time known length, stored in bytes.
  * `<forward-call>`: an argument is a parameter of the enclosing function
    passed on as-is (pointers and integers) or with a constant added or
    subtracted (integers only, `call-argdelta`). Each call site gets its own
    records, so several calls in one function are considered individually.

## How correlation is kept

The whole-program analysis groups these records into call sites keyed by
(called function id, file, line, column), so two calls on the same source
line are still different sites. The solver then resolves a sink relation
`(function, pointer arg, int arg, offset)`:

* At every level, the buffer and the index are resolved at the *same* call
  site. A site that does not determine both is skipped, never half-used, so
  the buffer of one call can never be paired with the index of another call.
* If both arguments are bound to constants at the site, the index
  (`value + accumulated offset`) is checked against the buffer size.
* If an argument is forwarded, the solver follows the `<forward-call>` of that
  same site into the caller, carrying the accumulated constant offset
  (`k` plus the `call-argdelta` values along the chain). Parameter positions
  may be swapped at each layer because every record maps its own argument
  number.
* Safe calls do not stop the search: if one call is in bounds, the remaining
  call sites are still examined, and the first chain that is definitely out
  of bounds is reported. A failed path is discarded together with its partial
  chain and cannot pollute sibling paths. Recursion and mutual recursion are
  bounded by `--max-ctu-depth`, like the existing CTU analysis.

The reported path shows the entry arguments, every forwarding layer and the
final array access, in the same display order as the existing CTU checks.

## Save and reload

`<unsafe-array-index>`, `<forward-call>` and `call-argvalue-known` are
serialized into the analyzer information files, so a scan through
`--cppcheck-build-dir` reloads the exact same relations and reaches the same
verdicts as a plain single-process scan. File information written by an older
cppcheck has none of these records; it is then analyzed with the old
capabilities only, and a missing delta/known-value attribute is never
interpreted as a fabricated zero index.

## Conservative boundaries

No definite error is produced from guessed values. The analysis gives up on:

* unknown integers, unknown array sizes, pointer casts or pointer arithmetic,
  and subscript expressions other than `p[i]`, `p[i+k]`, `p[k+i]`, `p[i-k]`;
* parameters that are modified before the access or the forwarding call, or
  that are handed to another function by modifiable reference or by address;
* conditional or loop contexts (the access must be unconditional straight-line
  code; no branch, loop or alias tracking is attempted);
* constant synthesis that would overflow: recorded values, deltas and their
  sums must stay inside the platform `int` range, otherwise the relation is
  dropped;
* call sites with conflicting known values for the same argument.

Negative subscripts and accesses exactly one element past the end are
reported; in-bounds accesses stay silent.
