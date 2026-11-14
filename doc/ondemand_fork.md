# On-Demand value forks (`value::fork()` / `forked_value`)

## Why forks exist

The On-Demand API is built around a single cursor: an `ondemand::value`,
`ondemand::object` or `ondemand::array` is just a window onto one shared
`json_iterator`. Copying a `value` copies a lightweight `value_iterator` that
still points at that same cursor, so two "copies" interleave their reads over
one position/depth/error state. Reading one out of the expected order fails
(`OUT_OF_ORDER_ITERATION` under `SIMDJSON_DEVELOPMENT_CHECKS`) and unescaped
strings share one parser-owned decode buffer.

That makes it awkward to feed one JSON sub-value to two independent extractors
while the parent loop keeps moving. A **fork** gives such a sub-value its own
independent cursor without rebuilding the document.

## The public API

```cpp
simdjson::ondemand::parser parser;
auto doc = parser.iterate(json);

for (auto row_value : doc["rows"]) {
  // Two independent forks of the still-unconsumed row value.
  auto price_fork = row_value.fork().value_unsafe();
  auto desc_fork  = row_value.fork().value_unsafe();

  // Parent keeps using the original cursor.
  auto id = row_value["id"].get_string().value_unsafe();

  // Each fork behaves like an ordinary ondemand::value.
  auto price = price_fork.get_value().value_unsafe();
  auto amount = price["price"]["amount"].get_double().value_unsafe();

  auto desc = desc_fork.get_value().value_unsafe();
  auto en = desc["desc"]["en"].get_string().value_unsafe();
}
```

- `value::fork() noexcept -> simdjson_result<forked_value>`.
- `forked_value::get_value() noexcept -> simdjson_result<value>`, callable
  **exactly once** per owner; a second call returns
  `OUT_OF_ORDER_ITERATION`.
- The returned `value` supports the whole usual surface: `get_object()`,
  `get_array()`, scalar reads, `find_field`/`operator[]`, `at()`,
  `at_pointer()`, range iteration, and may itself be `fork()`ed again.
- `forked_value` is **move-only** (not copyable) and default-constructible; a
  default-constructed instance holds no fork and its `get_value()` returns
  `UNINITIALIZED`.

## What is independent and what is shared

A fork owns, privately:

- its own token position and depth;
- its own `error_code` (a missing field, a wrong type or a lazily detected
  format error inside one fork never changes the parent or any sibling fork);
- its own string decode buffer, so unescaped strings produced by one fork, by a
  sibling fork and by the parent cannot overwrite one another;
- under `SIMDJSON_DEVELOPMENT_CHECKS`, its own per-depth container-start table.

A fork **shares**, read-only and without copying:

- the original input buffer;
- the stage-1 structural indexes;
- the `ondemand::parser` (used for the SIMD `parse_string`/number routines and
  the read-only index array).

No raw JSON is copied, stage 1 is never rerun, and no DOM tree is built. The
only per-fork allocations are its cursor, a decode buffer sized from the
sub-value, and (in checked builds) a depth table sized from the sub-value.

### Bounded to the sub-value

When a fork is created, the structural indexes of **that sub-value only** are
scanned to find its closing token. The fork cursor's end is parked one token
past that closing token, so fully consuming the fork can never read into the
next sibling. Scanning the sub-value's own indexes is explicitly allowed; the
scan does not depend on the size of any sibling.

### Cost does not scale with unrelated data

Because the boundary scan and every allocation are derived from the forked
sub-value alone, placing a much larger sibling before or after it does not
change the fork's cost. The contract test measures the real allocation bytes
made while building and reading a fixed small-object fork across documents
whose unrelated array grows by 100x; the measured cost is identical (and small)
in every case.

## Usage rules

### When forking is allowed

Call `fork()` only while the source value is still **unconsumed**. Inspecting
it with `type()` does not consume, so `type()` followed by `fork()` is legal.
Forking does not move the source cursor, so the same value may be forked more
than once and afterwards read or skipped normally by the parent path.

With `SIMDJSON_DEVELOPMENT_CHECKS=1`, forking a value that has moved off its
start returns `OUT_OF_ORDER_ITERATION`. Without the checks this is simply an
invalid call order, governed by the library's normal On-Demand usage contract,
and is not specially tracked.

### Lifetime contract (the fork does NOT extend anything)

The following must outlive every `forked_value`, and every value, object,
array and `string_view` obtained from one:

1. the original input buffer;
2. the original `ondemand::parser`;
3. the original `ondemand::document`.

While any fork is alive you must not call `parser.iterate()` again, nor
`rewind()` or move the original document/parser. The parent path is still free
to iterate **forward** normally.

Forks only guarantee their decoded `string_view`s until their owning
`forked_value` is destroyed; copy the bytes if you need them longer.

### Moving an owner

The cursor is held in a stable, heap-owned object. Moving a `forked_value`
moves the owning smart pointers but never relocates the cursor or the
allocations, so values and string views already handed out remain valid after
the move. Each of the moved-from and moved-to owners still obeys the
single-`get_value()` rule (the moved-from owner becomes empty).

### Nested forks

A value obtained from a fork may itself be forked. A nested fork borrows only
the original input, structural indexes and parser; it does not depend on the
outer fork's cursor. Once the nested fork exists, the outer `forked_value` may
be destroyed first, provided the original parser, document and input remain
alive.

### Threading

Forks support arbitrary **single-threaded** interleaving. They are not a
cross-thread mechanism; the underlying parser and document remain single-thread
objects.

## Error isolation

`INCORRECT_TYPE` and `NO_SUCH_FIELD` are non-fatal and, inside a fork, only
affect that fork's cursor. Fatal structural errors discovered lazily while a
fork walks malformed JSON are recorded in that fork's own `error_code`; the
parent iteration and sibling forks keep their own state and continue to work.
On-Demand remains lazy: a fork does not validate JSON it never reads, so only
errors the library actually reaches are reported.
