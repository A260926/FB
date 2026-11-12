# Resumable comparison-hint paging

`Prog.HintsPage` generates comparison-hint mutants in small, resumable
batches instead of running the whole enumeration in a single callback the
way `Prog.MutateWithHints` does. Both entry points share the same candidate
generation logic, traversal order (depth-first `ForeachArg`, increasing byte
offsets for plain data, 4-byte steps for compressed images, sorted and
deduplicated replacers per position), and validity checks, so they produce
the same sequence of candidates.

```go
func (p *Prog) HintsPage(callIndex int, comps CompMap, cursor []byte, limit int) (
    mutants []*Prog, next []byte, err error)
```

Each call returns at most `limit` candidates (`limit` must be positive).
Candidates that fail the standard validity checks do not occupy page slots.
If more candidates remain, `next` is a cursor that continues the
enumeration; `next` is empty once the enumeration is exhausted. An empty
page is returned only when the enumeration is exhausted.

## Saving and resuming

The cursor is an opaque byte string (at most 2048 bytes) that can be stored
in a file and reused in another process. It contains no program data,
candidate lists, or decompressed images — only hashes of the inputs and a
position. To resume, reload the original program, rebuild the comparison
table, and pass the saved cursor:

```go
// First process: generate the first page and save the continuation.
p, _ := target.Deserialize(progText, prog.Strict)
comps := collectComps(...) // prog.CompMap
mutants, next, err := p.HintsPage(0, comps, nil, 16)
if err != nil {
    log.Fatal(err)
}
execute(mutants)
os.WriteFile("prog.txt", p.Serialize(), 0o644)
os.WriteFile("hints.cursor", next, 0o644)

// Second process: reload and continue where the first one stopped.
progText, _ := os.ReadFile("prog.txt")
cursor, _ := os.ReadFile("hints.cursor")
p2, _ := target.Deserialize(progText, prog.Strict)
comps2 := collectComps(...) // the same comparisons; map insertion order does not matter
mutants, next, err := p2.HintsPage(0, comps2, cursor, 16)
```

The page size may change between pages: retrying with the same cursor and a
different `limit` yields the same candidates, only batched differently.

## When a cursor becomes invalid

A cursor is bound to:

- the target OS/arch,
- the full original program (all calls, not just `callIndex`),
- `callIndex`,
- the set of comparison operands and PCs in the comparison table.

If any of these change, `HintsPage` returns an error. Rebuilding the
comparison table with a different map insertion order is fine — only the
contents matter. Truncated, corrupted, oversized, or unsupported-version
cursors are also rejected with an error; `HintsPage` never silently
restarts the enumeration from the beginning. On any error, no partial page
is returned.

## Duplicate candidates

Replacers are deduplicated only within a single (argument, offset)
position. The same mutated program may legitimately appear again at a
different position (for example, the same 4-byte replacement at different
offsets of identical data). Such duplicates are expected and are not an
error; callers that execute the mutants may deduplicate on their own if
they care.

## Ownership and resource lifetime

- The source program, the comparison table, and the cursor passed to
  `HintsPage` are never modified.
- Every returned mutant owns an independent argument tree: mutating or
  discarding it does not affect the source program, other mutants of the
  same page, or mutants of subsequent pages.
- Compressed-image mutants follow the usual `pkg/image` conventions: the
  decompressed mapping is released before `HintsPage` returns (and, for
  `MutateWithHints`, before the user callback runs), so
  `image.StatImages`/`image.StatMemory` are back to their pre-call values
  after every call, including error and early-stop paths.
