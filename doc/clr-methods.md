# Static CLR method index

`lib/libpe/clr_methods.[ch]` adds a read-only ECMA-335 method index on top of
the existing byte-backed PE parser. The Common Language Runtime is never
loaded, assemblies are never resolved and CIL is never interpreted: every
result is derived from the on-disk metadata tables, the `#Strings`/`#Blob`
heaps and the managed method headers. Nothing here needs OpenSSL or a .NET
runtime.

## API

```c
pe_clr_methods_err_e pe_clr_methods_build(const pe_ctx_t *ctx,
    pe_clr_methods_t **out, pe_clr_methods_error_t *error);
size_t pe_clr_methods_query(const pe_clr_methods_t *index, uint32_t rva,
    pe_clr_query_hit_t *hits, size_t capacity);
void pe_clr_methods_free(pe_clr_methods_t *index);
```

* `pe_clr_methods_build` takes a context returned by `pe_parse`. A PE without
  a CLR data directory produces an **empty, successful** index. A PE that is
  managed but whose metadata or method bodies are invalid/unsupported fails
  with `*out == NULL` and a location-bearing `pe_clr_methods_error_t`
  (`code`, physical file `location`, static `message`); no partial index is
  ever returned.
* The index copies every string, signature, IL byte and position it exposes.
  Once built it is independent of the `pe_ctx_t` and its mapping, so the
  context can be released before the index is queried.
* `pe_clr_methods_query` reports every method whose **IL** half-open range
  contains an RVA. Pass a `NULL` buffer for a count-only call. The same RVA
  may match several methods (shared bodies); hits are returned in physical
  MethodDef RID order.

Error codes: `PE_CLR_METHODS_E_OK`, `PE_CLR_METHODS_E_ALLOCATION`,
`PE_CLR_METHODS_E_MALFORMED`, `PE_CLR_METHODS_E_UNSUPPORTED`.
`pe_clr_methods_error_msg()` maps a code to a static string.

## Supported metadata

* PE32 and PE32+, little-endian `BSJB` metadata root, one `#~` or `#-`
  tables stream plus `#Strings`, `#Blob` and `#GUID`.
* Nonzero rows are accepted in tables Module(0), TypeRef(1), TypeDef(2),
  FieldPtr(3), Field(4), MethodPtr(5), MethodDef(6), ParamPtr(7), Param(8),
  Assembly(32), AssemblyRef(35) and NestedClass(41). A nonempty other table
  is explicitly unsupported.
* Index widths are never hard-coded: heap columns follow the HeapSizes bits,
  simple indexes follow the target table's row count, and coded indexes
  (TypeDefOrRef over 2/1/27 and ResolutionScope over 0/26/35/1, two tag bits
  each) follow the largest participating row count.
* `#~` carries no pointer tables. In `#-`, a nonempty MethodPtr/ParamPtr is
  used instead of its target table and must be a complete permutation.

### Pointer-table ordering

`TypeDef.MethodList` slices the *effective* MethodPtr/MethodDef row space with
one-based starts; each logical item is then resolved through MethodPtr to a
physical MethodDef RID. Independently, `MethodDef.ParamList` is sliced in
**physical MethodDef row order** against the ParamPtr/Param space. Reordering
a method through MethodPtr therefore never reorders that physical method's
parameter endpoints. All exposed tokens use physical MethodDef/Param RIDs;
names are descriptive only and are never treated as identities.

* Empty lists, repeated adjacent list starts, zero-parameter methods and the
  count+1 end sentinel are normal. List starts must stay monotone in physical
  owner order.
* A Param row with Sequence 0 is the return-value metadata; it is retained as
  the first parameter record rather than renumbered.
* Nested names are resolved through NestedClass regardless of row order, with
  duplicate parentage and cycles rejected. Inner names are joined with `+`;
  only the outermost type is namespace-qualified, e.g. `Demo.Outer+Inner`.
* Method signatures are the complete `#Blob` payload after the compressed
  length prefix; the prefix itself is validated but not copied.

## Method bodies

RVA 0 methods stay in the index without a body. Nonzero bodies accept managed
tiny and fat headers only; native/runtime code types are unsupported.

* Tiny: the header is one byte and CodeSize is the upper six bits.
* Fat: the fixed 12-byte core is read first, then the header extent follows
  the actual DWORD count in the flags word (minimum three). MaxStack, the
  local variable signature token and the flags are copied.
* `MoreSects` trailing sections are neither parsed nor covered: query ranges
  and IL copies stop at the end of the CodeSize bytes.
* Every byte is mapped through explicit physical spans. A range may continue
  from one section into an adjacent virtual section whose raw offsets are not
  adjacent (this is how a fat header can cross a section boundary). Holes,
  ambiguous overlapping sections, virtual-only zero-fill tails and overflows
  are rejected instead of being guessed at; the permissive `pe_rva2ofs`
  fallback is never used to manufacture bytes.
* Metadata may live inside the PE headers as long as it is wholly within
  `SizeOfHeaders`.

Each record carries its TypeDef and MethodDef row spans, each parameter its
Param row span, and each body its header and IL spans (file offset, RVA, size
and 0-based source section; `PE_CLR_NO_SECTION` marks header-resident bytes).

## Example

`examples/clr_methods.c` takes a PE path and a **decimal** RVA and uses
`task_support/sample_io.h`, the dependency-free byte-backed entry point:

```
.clr_methods <pe-file> <decimal-rva>
```

The PE context is closed before anything is printed, demonstrating the
self-contained lifetime. Output is tab-separated UTF-8 with these records:

| Record     | Columns (after the record tag) |
|------------|--------------------------------|
| `METHOD`   | method token, RVA, name, flags, impl flags, TypeDef token, full type name, MethodDef row offset, row size, TypeDef row offset, row size |
| `SIGNATURE`| method token, payload size, hex payload |
| `BODY`     | method token, `tiny`/`fat`/`none`, header RVA, header size, IL RVA, IL size, MaxStack, local sig token, raw header flags (`none` uses placeholders) |
| `HDRSPAN`  | method token, file offset, RVA, size, section index (one line per physical run) |
| `ILSPAN`   | method token, file offset, RVA, size, section index (one line per physical run) |
| `IL`       | method token, hex IL bytes (header and trailing sections excluded) |
| `PARAM`    | method token, physical Param token, Sequence, flags, name, row offset, row size |
| `QUERY`    | queried RVA, hit method token, IL byte offset of the RVA |
| `UNCOVERED`| queried RVA, printed when no IL range matches |
| `QUERY_TRUNCATED` | total match count when the provided hit buffer was too small |

Method records appear in physical MethodDef RID order, parameters in logical
list order. Zero CodeSize never produces a `QUERY` hit; a shared body reports
one hit per method.

## Tests

`tests/clr_methods_cases.c` registers three `task_cases` groups driven by
`task_support/check.h`:

* **ownership** — `#~`/`#-` layout correspondence, MethodPtr/ParamPtr
  permutations, parameter ownership that does not follow method reordering,
  wide heaps, nested naming and physical row spans.
* **bodies** — tiny/fat header fields, the cross-section fat header, IL
  half-open boundary queries, shared and zero-size bodies and index survival
  after the PE context is freed.
* **integrity** — non-managed input, a corrupted metadata root, an
  unsupported table, a bad pointer permutation, a native body, an unbacked
  RVA and byte-for-byte input preservation on both successful and failing
  builds.

Run the full offline build and checks with `bash task_support/run.sh target`
(or `baseline` for the pre-existing directory-reading checks alone).
