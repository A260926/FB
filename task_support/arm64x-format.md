# ARM64X subset used by these synthetic fixtures

Reference: https://llvm.org/doxygen/COFFObjectFile_8cpp_source.html
(`Arm64XRelocRef::{getType,getSize,getValue,moveNext}`, `getHybridObjectView`).
The format is input documentation; no loader or target implementation is supplied.

The v1 table header is two LE uint32 values: Version and byte Size of the following records.
A PE32+ dynamic record is packed LE uint64 Symbol followed by uint32 BaseRelocSize and that many payload bytes.
For Symbol=6 the payload is a sequence of page blocks: uint32 page RVA, uint32 block size including the 8-byte header, then fixup records. Blocks have a 4-byte aligned size of at least 8; a zero uint16 at the end is padding.
Each nonzero fixup starts with uint16: low 12 bits are page-relative byte offset, bits12..13 are type, bits14..15 are argument.
Type0 ZEROFILL and type1 VALUE use argument1/2/3 for widths2/4/8. VALUE consumes that many following little-endian bytes; ZEROFILL has no payload.
Type2 DELTA always modifies a 32-bit target and consumes a uint16 magnitude. Bit14 selects subtraction when1, addition when0; bit15 selects scale8 when1, scale4 when0. Type3 is unsupported.
All source offsets refer to the original file. A fixup may target headers below SizeOfHeaders or a unique raw-backed section range. These samples do not change section layout, fixup metadata, image base or load-config location. No actual ARM64 or x64 instructions are interpreted.
