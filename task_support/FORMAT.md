# Public interface and material layout

Use ordinary C declarations in include/pdb.h:

```c
int pdb_get_modules(void *context, const struct pdb_module **modules, uint32_t *count);
int pdb_get_module_symbols(void *context, uint32_t module_index,
    const struct pdb_module_symbol **symbols, uint32_t *count);
int pdb_resolve_procedure_ref(void *context, uint32_t global_record_offset,
    struct pdb_procedure *out);
```

The new structures are normalized descriptors, not casts of host-sized CodeView structs.

- `pdb_module`: `uint32_t index`; borrowed `const char *name, *object_name`; `uint16_t stream_index`; `uint32_t symbol_bytes, c11_bytes, c13_bytes`.
- `pdb_module_symbol`: `uint32_t offset, size`; `uint16_t kind`; borrowed `const unsigned char *data`. Size includes record length and kind. Offset is measured from the module stream start, including its signature. The signature itself is not an enumerated symbol.
- `pdb_procedure`: `uint32_t module_index, symbol_offset`; borrowed `const char *name`; `uint16_t section`; `uint32_t address_offset, code_size, type_index`; `bool is_local`.

Return 0 on success, -1 with the existing context error on failure. Outputs on failure are cleared. No matching procedure kind gives `EPDB_UNSUPPORTED_VERSION`; invalid record/module identities give `EPDB_PARSE_ERROR`. Empty module lists or symbol lists succeed with count=0 and pointer=NULL. A module with stream_index=0xffff and zero symbol/C11/C13 lengths has no module stream. For a nonempty symbol region, the initial signature is CV_SIGNATURE_C13 (=4). The namespace of module indices is physical DBI order. An absent GSS has no procedure-reference records; trying to resolve one gives EPDB_NOT_FOUND. Returned strings and arrays use context lifetime. Existing successful calls and public symbol lookups retain their original ABI; new descriptors use fixed-width fields on LP64.

`image.c` builds MSF7 data in C, with two modules named same.obj, separate object paths, and procedures at byte offset 4. Their names match but their locality and section addresses differ. One S_PUB32 named public_marker is indexed by the ordinary PSI. Modes 0/1/2 select ordinary tables, missing GSI/PSI, and missing GSS as well. No machine code is present. All records use little-endian fixed-width encoding. The material is a starting example; add independent cases in the new test file.

Format references: [LLVM DBI](https://llvm.org/docs/PDB/DbiStream.html), [LLVM module stream](https://llvm.org/docs/PDB/ModiStream.html), [Microsoft reference-symbol description](https://github.com/microsoft/pdb-rs/blob/main/docs/pdb/globals.md). `SymByteSize` includes the four-byte signature. REFSYM2.imod is one-based; its ibSym is an unbiassed offset in the module stream. Global PSI/GSI hash offsets are a separate coordinate convention and must not be applied to ibSym.

`run.sh target` uses the shared counted test harness. The target test exports three required groups via task_cases/task_case_count and uses TASK_CHECK at actual comparisons. It compiles production src/*.c except the existing pdbparse CLI. If adding production files, keep the upstream CMake targets usable as well. The direct build uses PDB_ENABLE_ASSERTIONS=1, matching upstream's default, and defines __unix__ for POSIX host selection. It needs only cc, ar and bash; CMake documentation tooling is not a runtime dependency.

The existing pdbparse consumer gains --modules. Output must identify physical module index and object path on each module, and identify global-record offset, target module index, target symbol offset, and section:offset on each resolved reference. Names alone are insufficient. Exact spacing is free; document the format and compare parsed identities in the new tests. Existing -f/-S/-s remain composable and unchanged when --modules is absent. New test group optional_streams runs the real .task-build/pdbparse process on temporary C-generated PDB files; preserve child exit codes and verify content.

Without PSI, global-record enumeration and S_PUB32 enumeration still inspect the actual GSS. Public name lookup falls back to a scan, preserving the existing case_sensitive option. Without GSS both enumerations are empty and lookup returns EPDB_NOT_FOUND. Module symbol access must not invoke the optional global/public parser first.
