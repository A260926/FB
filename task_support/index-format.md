# Scope and primary format references

This is a little-endian MSF7 PDB, DBI version 19990903. DBI offsets12/16/20 hold GSI/PSI/symbol-record stream indices. A missing optional index is 0xffff.

GSI scope: v7 signature0xffffffff, version0xf12f091a, 4096 buckets (no MinimalDebugInfo feature). Four uint32 header fields: signature, version, hash-record byte size, bucket-region byte size. Serialized hash records are two uint32 fields (symbol offset biased by+1, reference count), followed by a 516-byte bitmap and one uint32 chain-start for each occupied bucket. Starts use the historical12-byte in-memory scale, not the serialized8-byte stride. Hash V1 chooses the bucket; names still need exact byte comparison.

PSI scope: 28-byte header; SymHash and AddrMap give the byte lengths of its nested GSI table and subsequent uint32 symbol-offset array. The latter offsets are zero-based. Entries are sorted by (public symbol section, offset, name). Same-address aliases are separate records. Samples have no thunk or section-offset tables; report unsupported when these are declared rather than ignoring them.

Primary sources:
- Microsoft format notes: https://github.com/microsoft/pdb-rs/blob/main/docs/pdb/globals.md
- LLVM GSI serialization: https://www.llvm.org/docs/doxygen/GSIStreamBuilder_8cpp_source.html
- LLVM public stream reader: https://llvm.org/doxygen/PublicsStream_8cpp_source.html
- LLVM hash routines: https://llvm.org/doxygen/Hash_8cpp_source.html

fixture.rs only writes known input records and fixed bucket numbers. It does not implement index lookup. Stream5 contains five distinct symbols. Streams6/7 encode the two index families. The fixture stays entirely in memory; no PE is loaded or executed.
