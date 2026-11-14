# Split BTF views and the union declaration generator

This document describes the split-view API added to `ibtf.h`, the
multi-module header generator in `ibtfheadergenerator.h`, and the forensic
type inspector in `ibtfinspector.h`. Everything here is read-only: BTF files
are parsed from disk, no kernel module is loaded, and no image is executed.

## Scope and numbering

A **root view** is produced by `IBTF::createFromPath` or
`IBTF::createFromPathList`. It owns the full type map and string table of the
files it was given.

A **split view** is produced by

```cpp
static Result<Ptr, BTFError>
IBTF::createSplitFromPath(const IBTF &base, const std::filesystem::path &path);
```

The child view exposes three groups of types:

1. **Ancestors.** Every type of `base` stays visible under its original ID.
   The ancestor records are copied into the child, so the child remains valid
   after `base` is destroyed.
2. **Local types.** The child's own types occupy IDs immediately above the
   highest ancestor ID. If the base exposes IDs `1..N`, the child's first
   local type is `N + 1`. A sibling view of the same base numbers its types in
   the same range, so the same numeric ID can mean different things in two
   siblings. Each type therefore carries provenance, not just an ID.
3. **Void.** ID `0` is always void and has no record, as in plain BTF.

`getType`, `getKind`, `getAll` and `count` behave like the linear interface:
`count()` includes ancestors and local types, `getAll()` returns the merged
map, and unknown IDs return `std::nullopt`.

### Provenance

`getTypeScope(id)` returns a `BTFTypeScope`:

| field    | meaning                                                        |
|----------|----------------------------------------------------------------|
| `source` | opaque, process-unique token of the load that introduced the type |
| `depth`  | `0` for the root load, `+1` per split layer                    |
| `id`     | the ID under which the queried view exposes this type          |

`(source, id)` is the identity of a type across views. Names are never used to
infer identity. Tokens are never reused, so two independent loads of
byte-identical files remain distinct even after one is freed.

`getOriginToken()` identifies the view's own load; `getDepth()` returns the
scope depth of that load.

### Strings

A base cannot keep only decoded names. A child may use strings from the
ancestor string table that no ancestor type ever referenced (for example
`ghost_name` in the samples). The child therefore retains the ancestor string
bytes:

- `getStringSection()` returns the global string bytes owned by the view, in
  the same offset space as its type records.
- A split view's section is the ancestor section followed by its own file's
  string table, so offsets used by the child and by any future grandchild
  resolve identically.
- Because the bytes are owned by the view, the source file may be deleted
  after parsing, and the parent handle may be destroyed after the child is
  created. A grandchild can still be loaded from the child.

The plain linear interface (`createFromPathList`) is unchanged: it still
concatenates string tables in file order and numbers types from `1`.

### Type references in a split file

Split BTF records reference types by global ID. A reference either names an
ancestor ID (kept verbatim) or one of the child's own IDs (rewritten into the
child's numbering). `detail::forEachTypeReference` in
`btfparse/btfparse/src/btftypereferences.h` enumerates every reference field
for every kind: pointers, array element and index types, struct/union
members, typedef, const/volatile/restrict, function return and parameters,
`Var.type` and `DataSec` variable types. A reference that resolves to neither
an ancestor nor a local type is rejected with
`InvalidSplitTypeReference`; nothing is silently tolerated.

## Multi-module header generation

```cpp
virtual bool generateModules(
    std::string &header,
    const std::vector<std::pair<std::string, const IBTF *>> &modules) const;
```

The input is the root view plus each source layer, each with a unique module
name. Names must be valid C identifiers (`[A-Za-z_][A-Za-z0-9_]*`) and may not
repeat.

### Declaration naming

- Every struct, union, enum, forward declaration and typedef is emitted as
  `<module>__<original name>`. The owning module is the one whose load
  introduced the type, so an ancestor declared once by the base is named
  `base__shared` even when both siblings inherit it.
- Enum members are emitted as `<module>__<enum>__<member>` so enumerators,
  which share one C namespace, cannot collide across modules.
- Fundamental integer and float spellings (`int`, `unsigned char`, …) are
  left untouched. They are never emitted as top-level declarations, and
  duplicate fundamental names across modules are accepted.

### What is declared once

Types are merged by provenance `(source, id)`, not by name or by bytes:

- Types inherited from one base load are declared exactly once.
- Two independent loads of byte-identical files have different tokens and
  are never merged; each is declared under its own module prefix.

### Reference rewriting

Each record is rewritten once, using the numbering of the view that
contributed it. A pointer inside the left `node` therefore points at
`left__node`, and the right pointer points at `right__node`, even though both
were local ID 3 in their own layer. Recursive self-pointers, embedded
ancestor structs, arrays and function prototypes all resolve through the same
map.

### Order independence

Output does not depend on the order of the `modules` argument: internal
ordering is by module name, and a type's prefix comes from the load that
introduced it. Passing `{base, left, right}` or `{right, left, base}` produces
byte-identical headers.

### Rejections

`generateModules` returns `false` and leaves `header` empty (never a partial
header) when:

- `modules` is empty;
- a module name is not a valid C identifier, or a name repeats;
- a view is null;
- a view cannot report provenance;
- a type inherited from a load has no supplied source view (for example,
  passing only `left` without the `base` it inherited from);
- a reference cannot be resolved in the contributing view;
- two distinct declarations would receive the same name.

The single-view `generate` API is unchanged.

## Forensic type inspection

```cpp
Result<BTFTypeInspection, BTFError>
inspectSplitType(const IBTF &view, const std::string &module_name,
                 std::uint32_t local_type_id,
                 const std::vector<std::uint8_t> &object_bytes);
```

This resolves a module-local type through a production split view and reports:

- the module name, requested and resolved ID, source token and depth;
- the struct/union size, kind and display name;
- one record per fixed member: name, member type ID, **type source and
  depth**, display spelling, bit offset, size, pointer flag and stored pointer
  value, and the raw bytes covered by the member.

Pointers are reported as the value stored in the supplied bytes (decoded as
little-endian). They are never followed and no kernel memory is read.

### Identity, not names

Two modules may both call their local ID 4 `node` with different layouts. The
view's provenance decides how the bytes are interpreted. If the supplied
object length does not equal the selected view's declared size, the call fails
with `ObjectSizeMismatch` and a message that states both lengths; it never
falls back to another module's layout or merges types by name.

### Unexplainable layouts

`explainable` is `false`, with a reason, when a member cannot be laid out:

- unsupported **bitfields** (recorded as bitfields, with no byte slice);
- **incomplete** by-value types (forward-declared tags, void, function
  types), which also produce no byte slice.

The inspection still records the size, provenance and member list so the
caller can log why it refused to interpret the object. The generated C
declarations remain useful for cross-checking layout, but they do not replace
the source identity recorded here.

## Example

`examples/split_modules.cpp` loads a base plus two sibling files through the
production API and prints the union declaration:

```sh
split_modules base.btf left.btf right.btf
```

It also accepts a forensic request — module name, local type ID, hex bytes:

```sh
split_modules base.btf left.btf right.btf left 4 1122...   # 16-byte object
split_modules base.btf left.btf right.btf right 4 1122...  # 24-byte object
```

The left object is 16 bytes and the right object is 24 bytes; submitting the
right bytes under `left 4` reports the size mismatch and exits non-zero.
