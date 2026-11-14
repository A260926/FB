Synthetic CompactDex v001, read only

`Samples.image(false/true)` produces the supplied ordinary.dex and minimal.cdex.
They describe LA;.f()I, source A.java, one register, const/4 7 and return,
and line 42 at address zero. These are inert format data; never execute them.
The fixture is a starting example, not coverage for the requested new reader.

CompactDex references: Android ART libdexfile/dex/compact_dex_file.h
(CodeItem and Header), compact_offset_table.h/.cc (Accessor), dex_file-inl.h.
https://android.googlesource.com/platform/art/+/refs/heads/main/libdexfile/dex/

v001 header size is 0x88. After the ordinary header are six uint32 fields:
feature_flags, debug_info_offsets_pos, debug_info_offsets_table_offset,
debug_info_base, owned_data_begin, owned_data_end. ID tables are in the main
region; offsets to strings, type lists, annotations, class data, code, debug
and map data address the data region beginning at data_off.
The sparse debug table has one lookup entry per 16 method IDs. A block starts
with a big-endian 16-bit presence mask, then unsigned LEB128 deltas. The lookup
entries are little-endian uint32 offsets relative to debug_info_offsets_pos.
A zero bit means absent; each block's running offset starts at debug_info_base.

Compact code consists of two uint16 fields followed by instructions. The four
nibbles of the first field encode local registers, incoming words, outgoing
words and tries. The second has instruction count in the upper eleven bits
and preheader flags in the lower five. Extensions precede the header; the
instruction-count extension uses two uint16 words, other fields one each.
Total registers include incoming words. Exception offsets are in code units;
try items and encoded handlers retain DEX meanings. Debug offsets come from
method identity rather than from code_item. The ART definition is authoritative.

The new regression class is absent intentionally. Use Check.group(name) and
Check.check(condition,message) for code/debug/integration; its main is called
by Check.run so missing, empty and early-return classes cannot claim success.
The build uses only javac/java, compiling the current dex-reader-api and
reader sources, including any new production .java files.
