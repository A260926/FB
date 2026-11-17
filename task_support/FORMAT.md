# Android APS2 wire contract

Source: Android bionic `linker/linker_reloc_iterators.h`,
https://android.googlesource.com/platform/bionic/+/ec00401/linker/linker_reloc_iterators.h
This note defines data semantics, not an implementation.

Four literal bytes `APS2`, then signed LEB128 integers: relocation_count,
initial_offset, followed by groups until relocation_count records are produced.
Each group starts with group_size and group_flags. Bits are INFO=1,
OFFSET_DELTA=2, ADDEND=4, HAS_ADDEND=8. Unknown bits, zero/negative group_size,
ADDEND without HAS_ADDEND, or any addend flag in REL are errors.
A group header next contains offset_delta if bit 2, r_info if bit 1, then
addend_delta if bits 4 and 8. Grouped addend_delta updates running addend once
before its records; each record uses that same addend. Without bit 8 the
running addend resets to zero. With bit 8 but not bit 4 each record has its own
addend_delta, updating the running addend. Running offset always persists.
Each record contributes offset_delta if not bit 2, r_info if not bit 1,
then addend_delta if bit 8 and not bit 4, in that order.
SLEB128 byte order does not depend on ELF byte order. Offset and r_info are
unsigned ELF-width bit patterns encoded through the corresponding signed
integer; addends are signed ELF-width values. Mathematical offset updates
must stay in the unsigned file-width range, addend updates in the signed
file-width range. Each integer occupies at most ceil(width/7) bytes with valid
sign extension. High-bit offset/r_info patterns use their signed file-width
representation in SLEB128, then become unsigned values. Counts/flags are nonnegative. No trailing
bytes are permitted. Empty streams with count zero still include initial_offset.

Fixtures are inert ELF data. Decoding means explaining symbolic references;
none of their contents are mapped executable or invoked.
