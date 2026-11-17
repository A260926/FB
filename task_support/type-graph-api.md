# Read-only procedure type graph contract

The data is metadata-only MSF7, TPI/IPI v20040203, modern zero-terminated CodeView records. No PE loading, type-server lookup, cross-module imports, instruction execution, or writeback is involved.

Add exported `TypeReference::{Type(TypeIndex), Id(IdIndex)}` (Copy, Eq) and a `type_reference: TypeReference` field to `ProcedureSymbol`. Keep `type_index` with its existing raw bits for compatibility. Ordinary GPROC32/LPROC32, their ST forms and LPROC32_DPC use Type; GPROC32_ID/LPROC32_ID/LPROC32_DPC_ID use Id. Existing parse and iter APIs continue working. Updating expected struct literals in old unit tests is allowed; do not remove old assertions.

Add `PDB::type_graph(&mut self) -> Result<TypeGraph>`. The graph owns the information needed from both streams and remains usable after that PDB, its module views and source are dropped. Use the existing type/id parsers, not a parallel CodeView decoder. `TypeGraph` provides:

- `resolve_type(&mut self, TypeIndex) -> Result<TypeNodeId>` for a TPI root.
- `resolve_procedure(&mut self, &ProcedureSymbol<'_>) -> Result<TypeNodeId>` for a procedure signature root.
- `node(&self, TypeNodeId) -> Option<&TypeNode>`.

`TypeNodeId` is opaque, Copy + Eq + Debug, local to its graph. It identifies a shared node rather than a traversal occurrence. IDs already returned by a graph never change. No static/global cache keyed only by an integer type index. Cross-graph numeric ID equality is not part of the API; inspect each graph's own nodes to verify isolation.

The public `TypeNode` has `index: TypeIndex` and `kind: TypeNodeKind`. Public variants and fields:

```rust
Primitive { index: TypeIndex }
Pointer { to: TypeNodeId, attributes: PointerAttributes }
Modifier { to: TypeNodeId, constant: bool, volatile: bool, unaligned: bool }
Procedure { return_type: Option<TypeNodeId>, arguments: Vec<TypeNodeId>, attributes: FunctionAttributes }
MemberFunction { return_type: TypeNodeId, class: TypeNodeId, this_pointer: Option<TypeNodeId>, arguments: Vec<TypeNodeId>, attributes: FunctionAttributes, this_adjustment: u32 }
Class { kind: ClassKind, name: Vec<u8>, unique_name: Option<Vec<u8>>, size: u64, complete: bool, members: Vec<TypeMember> }
```

`TypeMember` has owned `name: Vec<u8>`, `offset: u64`, `ty: TypeNodeId`, and `attributes: FieldAttributes`. Preserve raw name bytes, source member order, qualifiers, pointer attributes, calling convention and `this` adjustment. This is a structural graph, not a C declarator pretty-printer. LF_ARGLIST and LF_FIELDLIST are internal lists; they are not separate public graph nodes. Support fixed-arity LF_PROCEDURE/LF_MFUNCTION, LF_ARGLIST, LF_POINTER (ordinary pointer/lvalue/rvalue reference, not pointer-to-member), LF_MODIFIER, LF_CLASS/LF_STRUCTURE with data members and LF_INDEX, and all primitive encodings already supported by the library. Classes have no bases or vtable metadata in this scope. Reached, valid unsupported kinds return a contextual error; unrelated unsupported records do not prevent graph creation or supported queries.

Procedure Type references resolve directly to TPI LF_PROCEDURE/LF_MFUNCTION. Id references accept IPI LF_FUNC_ID and LF_MFUNC_ID, take their TPI `function_type`, then require the corresponding procedure/member-function kind. The member Id's `parent` and member-function's class type must canonicalize to the same class. Never retry an IPI failure against a same-numbered TPI record. IPI `scope` is not an extra type edge. Missing IPI is acceptable for direct TPI queries and yields a clear IPI error only when an ID procedure is requested.

Only class/struct forward references are aliases. With a unique name, select complete definitions with the same class kind and exact unique-name bytes, ignoring display-name spelling. Without one, use kind plus exact display name. Match against complete definitions only. One candidate aliases that complete node; zero candidates produces a stable incomplete node with no members; more than one is an ambiguity error listing the forward index and candidate indices. Do not merge explicitly referenced complete definitions, even if they have identical names. The complete definition's index is the canonical node index. Class and struct are different kinds. The policy for matching a forward without a unique name includes all complete definitions with the same display name, including those that do have a unique name.

A successful resolution follows the entire supported reachable graph. Pointer recursion and mutual class recursion are legal and preserve shared identity. Follow LF_INDEX in list order, with its required zero `u16` padding preceding the `u32` type index (Microsoft `lfIndex`, not the legacy 16t variant). Repeated references to an already resolved field type reuse its node. No fixed print-depth cutoff or placeholder may remain in a successful graph. If resolution fails for a valid ambiguous/unsupported reachable type, existing successful nodes stay intact; a later unrelated supported query must work and cannot observe a half-built node. New nodes from a failed query need not retain their IDs, since those IDs were never returned.

Regression file: `tests/procedure_types.rs`, four top-level tests named `origin`, `forward`, `recursive`, `compatibility`. Import `task_support/check.rs` by path; each test calls `run` once with the matching name and uses `Check::check` for actual value assertions. Do not print completion markers yourself. `target` recompiles the current crate and runs all four tests, rejects missing/ignored/zero-assertion/early-exit groups, then runs the baseline. Expand the Rust fixture builders as needed. Do not add dependencies.

Format reference: Microsoft `microsoft-pdb`, cvinfo.h at 082c5290e5aff028ae84e43affa8be717aa7af73, `lfIndex`, `lfProc`, `lfMFunc`, `lfFuncId`, `lfMFuncId`, and `PROCSYM32`.
https://github.com/microsoft/microsoft-pdb/blob/082c5290e5aff028ae84e43affa8be717aa7af73/include/cvinfo.h
