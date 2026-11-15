# Type-Use Annotation Support

This document describes the scope of Procyon's support for the
`RuntimeVisibleTypeAnnotations` and `RuntimeInvisibleTypeAnnotations` attributes defined in
JVMS 4.7.20.

## Data sources and guarantees

Type-use annotations are read directly from class file bytes. The decompiler never loads,
initializes or executes an input class, never runs annotation processors, and never derives a
type annotation from decompiled text or an annotation's simple name.

Every decoded entry preserves:

- the owning declaration (class, field, method, formal parameter, record component);
- the raw `target_type` as a typed enum (`TypeAnnotationTargetKind`);
- the full `target_info` union (`TypeAnnotationTargetInfo`);
- the raw ordered `type_path` (`TypeAnnotationPath`), including an explicit empty path;
- visibility (`RUNTIME` vs `CLASS`);
- the complete annotation value with its original member order.

Declaration annotations (`RuntimeVisibleAnnotations` / `RuntimeInvisibleAnnotations`) continue
to be handled separately and are never mixed with type-use annotations.

## Targets emitted into decompiled Java source

The following targets are reconstructed and written back into source:

| Target type | `target_type` | Placement |
|---|---|---|
| Class type parameter | `0x00` | type parameter declaration |
| Method type parameter | `0x01` | type parameter declaration |
| Superclass / interface | `0x10` (`65535` = superclass) | `extends` / `implements` type |
| Class type parameter bound | `0x11` | type parameter bound (incl. intersection members) |
| Method type parameter bound | `0x12` | type parameter bound (incl. intersection members) |
| Field | `0x13` | field type usage |
| Method return | `0x14` | method return type |
| Formal parameter | `0x16` | parameter type usage |
| Throws | `0x17` | `throws` type |

### `type_path` interpretation

All four path kinds are supported and may be combined:

- `ARRAY_ELEMENT (0)` — walks array dimensions; the final position determines whether the
  annotation is attached to a specific bracket or, once all dimensions are crossed, the
  component type. An empty path on an array type names its outermost dimension.
- `INNER_TYPE (1)` — advances one nested-class segment of a dotted member type.
- `WILDCARD_BOUND (2)` — moves from a wildcard (`?`) to its extends/super bound.
- `TYPE_ARGUMENT (3)` — selects an indexed generic type argument.

An out-of-range index or a path step with no corresponding node in the reconstructed type
tree raises a placement error; an annotation is never silently moved to an outer type.

## Targets retained as metadata but not emitted

These targets are fully decoded, validated and available for queries, but are not written into
decompiled source:

| Target type | `target_type` | Reason |
|---|---|---|
| Method receiver | `0x15` | no source position in the reconstructed method header |
| Local variable / resource | `0x40`, `0x41` | Code-attribute target |
| Exception parameter | `0x42` | Code-attribute target |
| `instanceof` | `0x43` | Code-attribute target |
| `new` | `0x46`, `0x44`, `0x45` | Code-attribute target |
| Cast / invocation type arguments | `0x47`–`0x4B` | Code-attribute target |

Their `target_info` (including local-variable table rows and bytecode offsets) is read
completely so one entry cannot cause a following entry to be misread.

## Known structural limitation

Procyon's reconstructed `SimpleType` keeps only the innermost segment's generic type arguments;
the type arguments of a parameterized *enclosing* class are not represented in the AST (this is
a pre-existing limitation independent of annotations — it also affects unannotated non-static
inner generics). A type annotation addressed to such an owner-segment argument is therefore:

- still decoded and retained on the declaration's queryable metadata
  (`Keys.UNSUPPORTED_TYPE_ANNOTATIONS`);
- reported as an **unanalyzable** entry by the audit example;
- never attached to the wrong (inner) segment.

## Formal parameter mapping

`formal_parameter_index` is a source-level index, not a descriptor slot. Hidden compiler
parameters are reconciled using the `MethodParameters` flags together with the method signature
and the declaring class's inner-class/enum nature:

- a non-static inner-class constructor's mandated enclosing instance;
- an enum constructor's synthetic `$enum$name` / `$enum$ordinal`;
- other synthetic/mandated parameters.

When a source parameter cannot be identified uniquely, an explicit unsupported error is raised
rather than guessing.

## Records and varargs

- Record component annotations are taken from the component's own attributes; the copies javac
  places on the generated field, constructor and accessor are not duplicated onto the component
  header. An explicitly declared accessor whose return-type annotations differ from the
  component is retained instead of being collapsed.
- For a varargs parameter, the last array dimension is rendered as an ellipsis (`...`) and the
  annotation belonging to that dimension follows it; earlier dimensions remain brackets.

## Recompilation

Decompiled source is expected to recompile with `javac --release 17 -proc:none`. The test suite
verifies equivalence by comparing decoded metadata (target, `type_path`, visibility and
annotation type) between the original and recompiled classes — not by comparing source text.
