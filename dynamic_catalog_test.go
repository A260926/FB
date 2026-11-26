package elf_reader

// Tests for ReadDynamicImports. All images are synthetic, hand-assembled
// byte buffers: they contain program headers but no section table, like
// stripped shared libraries.

import (
	"bytes"
	"encoding/binary"
	"strings"
	"testing"
)

// The virtual addresses at which the pieces of a synthetic image are
// mapped. Each piece gets its own PT_LOAD segment (except the dynamic
// array, which gets the PT_DYNAMIC segment).
const (
	catStrtabVA  = 0x400000
	catVersymVA  = 0x401000
	catVerneedVA = 0x402000
	catHashVA    = 0x403000
	catGNUHashVA = 0x404000
	catSymtabVA  = 0x405000
	catDynamicVA = 0x406000
)

// dynSymSpec describes one symbol to place in a synthetic dynamic symbol
// table. An empty name produces a zero st_name field.
type dynSymSpec struct {
	name    string
	binding uint8
	typ     uint8
	shndx   uint16
}

// vernauxSpec describes one version need aux record.
type vernauxSpec struct {
	hash  uint32
	flags uint16
	other uint16
	name  string
}

// verneedSpec describes one version need record and its aux chain.
type verneedSpec struct {
	file string
	aux  []vernauxSpec
}

// gnuHashSpec describes the variable parts of a synthetic DT_GNU_HASH table.
type gnuHashSpec struct {
	symoffset uint32
	buckets   []uint32
	chain     []uint32
}

// dynEnt is one raw dynamic table entry; DT_NULL is appended automatically.
type dynEnt struct {
	tag   uint64
	value uint64
}

// segmentSpec describes one program header and its file-backed content.
type segmentSpec struct {
	typ   uint32 // 0 means PT_LOAD
	flags uint32
	vaddr uint64
	data  []byte
	memsz uint64 // 0 means len(data)
}

// catalogImageConfig configures buildCatalogImage.
type catalogImageConfig struct {
	bo                 binary.ByteOrder // nil means little-endian
	syms               []dynSymSpec
	versym             []uint16      // nil means no DT_VERSYM
	verneed            []verneedSpec // nil means no DT_VERNEED
	verneedNum         *uint64       // nil means len(verneed)
	useHash            bool
	hashNchainOverride uint32 // 0 means len(syms)
	gnu                *gnuHashSpec
	extraDynamic       []dynEnt
	symtabSplit        int // split the symbol table across two segments
	strtabAddrOverride uint64
	symtabAddrOverride uint64
	strsz              *uint64 // nil means the string table size
	syment             *uint64 // nil means 24
	extraSegments      []segmentSpec
}

// Builds a string table: a leading zero byte followed by the
// null-terminated strings (deduplicated). Returns the table and the offset
// of each string.
func buildStringTable(strs []string) ([]byte, map[string]uint32) {
	offsets := make(map[string]uint32)
	data := []byte{0}
	for _, s := range strs {
		if _, exists := offsets[s]; exists {
			continue
		}
		offsets[s] = uint32(len(data))
		data = append(data, s...)
		data = append(data, 0)
	}
	return data, offsets
}

// Serializes a dynamic symbol table.
func buildSymbolBytes(bo binary.ByteOrder, syms []dynSymSpec,
	nameOffsets map[string]uint32) []byte {
	out := make([]byte, 24*len(syms))
	for i, s := range syms {
		p := out[i*24:]
		if s.name != "" {
			bo.PutUint32(p[0:4], nameOffsets[s.name])
		}
		p[4] = s.binding<<4 | s.typ
		bo.PutUint16(p[6:8], s.shndx)
	}
	return out
}

// Serializes a DT_HASH table. Only the header fields matter for symbol
// counting, so buckets and chains are left zeroed.
func buildELFHash(bo binary.ByteOrder, nbucket, nchain uint32) []byte {
	out := make([]byte, 8+4*nbucket+4*nchain)
	bo.PutUint32(out[0:4], nbucket)
	bo.PutUint32(out[4:8], nchain)
	return out
}

// Serializes a DT_GNU_HASH table with a single bloom word.
func buildGNUHashBytes(bo binary.ByteOrder, spec *gnuHashSpec) []byte {
	out := make([]byte, 16+8+4*len(spec.buckets)+4*len(spec.chain))
	bo.PutUint32(out[0:4], uint32(len(spec.buckets)))
	bo.PutUint32(out[4:8], spec.symoffset)
	bo.PutUint32(out[8:12], 1) // one 8-byte bloom word
	bo.PutUint32(out[12:16], 0)
	offset := 24
	for _, b := range spec.buckets {
		bo.PutUint32(out[offset:], b)
		offset += 4
	}
	for _, c := range spec.chain {
		bo.PutUint32(out[offset:], c)
		offset += 4
	}
	return out
}

// Serializes a DT_VERSYM table.
func buildVersymBytes(bo binary.ByteOrder, values []uint16) []byte {
	out := make([]byte, 2*len(values))
	for i, v := range values {
		bo.PutUint16(out[i*2:], v)
	}
	return out
}

// Serializes a DT_VERNEED table. All verneed headers come first, followed
// by each record's aux chain, so the relative vn_aux offsets differ from
// record to record and never coincide with absolute file positions.
func buildVerneedBytes(bo binary.ByteOrder, needs []verneedSpec,
	nameOffsets map[string]uint32) []byte {
	total := 16 * len(needs)
	for _, n := range needs {
		total += 16 * len(n.aux)
	}
	out := make([]byte, total)
	auxOffset := 16 * len(needs)
	for i, n := range needs {
		header := out[i*16:]
		bo.PutUint16(header[0:2], 1) // vn_version
		bo.PutUint16(header[2:4], uint16(len(n.aux)))
		bo.PutUint32(header[4:8], nameOffsets[n.file])
		// vn_aux is relative to this header's position.
		bo.PutUint32(header[8:12], uint32(auxOffset-i*16))
		if i+1 < len(needs) {
			bo.PutUint32(header[12:16], 16) // vn_next
		}
		for j, a := range n.aux {
			p := out[auxOffset:]
			bo.PutUint32(p[0:4], a.hash)
			bo.PutUint16(p[4:6], a.flags)
			bo.PutUint16(p[6:8], a.other)
			bo.PutUint32(p[8:12], nameOffsets[a.name])
			if j+1 < len(n.aux) {
				bo.PutUint32(p[12:16], 16) // vna_next
			}
			auxOffset += 16
		}
	}
	return out
}

// Serializes a dynamic table, appending the DT_NULL terminator.
func buildDynamicBytes(bo binary.ByteOrder, entries []dynEnt) []byte {
	out := make([]byte, 16*(len(entries)+1))
	for i, e := range entries {
		bo.PutUint64(out[i*16:], e.tag)
		bo.PutUint64(out[i*16+8:], e.value)
	}
	return out
}

// Assembles a complete 64-bit ELF image (header, program headers, and
// segment contents) with no section table.
func assembleImage(bo binary.ByteOrder, segs []segmentSpec) []byte {
	offset := uint64(64 + 56*len(segs))
	fileOffsets := make([]uint64, len(segs))
	for i := range segs {
		if offset%16 != 0 {
			offset += 16 - offset%16
		}
		fileOffsets[i] = offset
		offset += uint64(len(segs[i].data))
	}
	raw := make([]byte, offset)
	raw[0] = 0x7f
	raw[1] = 'E'
	raw[2] = 'L'
	raw[3] = 'F'
	raw[4] = 2 // 64-bit class
	if bo == binary.BigEndian {
		raw[5] = 2
	} else {
		raw[5] = 1
	}
	raw[6] = 1                     // version
	bo.PutUint16(raw[16:18], 3)    // shared object
	bo.PutUint16(raw[18:20], 0x3e) // AMD64
	bo.PutUint32(raw[20:24], 1)    // version
	bo.PutUint64(raw[32:40], 64)   // program header offset
	bo.PutUint16(raw[52:54], 64)   // ELF header size
	bo.PutUint16(raw[54:56], 56)   // program header entry size
	bo.PutUint16(raw[56:58], uint16(len(segs)))
	bo.PutUint16(raw[58:60], 64) // section header entry size
	// The section header count and names index stay zero: no section table.
	for i, s := range segs {
		typ := s.typ
		if typ == 0 {
			typ = LoadableSegment
		}
		memsz := s.memsz
		if memsz == 0 {
			memsz = uint64(len(s.data))
		}
		ph := raw[64+i*56:]
		bo.PutUint32(ph[0:4], typ)
		bo.PutUint32(ph[4:8], s.flags)
		bo.PutUint64(ph[8:16], fileOffsets[i])
		bo.PutUint64(ph[16:24], s.vaddr)
		bo.PutUint64(ph[24:32], s.vaddr)
		bo.PutUint64(ph[32:40], uint64(len(s.data)))
		bo.PutUint64(ph[40:48], memsz)
		bo.PutUint64(ph[48:56], 0x1000)
		copy(raw[fileOffsets[i]:], s.data)
	}
	return raw
}

// Builds a complete synthetic image holding the given dynamic linking
// information.
func buildCatalogImage(cfg catalogImageConfig) []byte {
	bo := cfg.bo
	if bo == nil {
		bo = binary.LittleEndian
	}
	// Collect the dynamic string table's strings: symbol names first, then
	// version need file and version names.
	var strs []string
	for _, s := range cfg.syms {
		if s.name != "" {
			strs = append(strs, s.name)
		}
	}
	for _, n := range cfg.verneed {
		strs = append(strs, n.file)
		for _, a := range n.aux {
			strs = append(strs, a.name)
		}
	}
	strtab, nameOffsets := buildStringTable(strs)
	strtabAddr := uint64(catStrtabVA)
	if cfg.strtabAddrOverride != 0 {
		strtabAddr = cfg.strtabAddrOverride
	}
	symtabAddr := uint64(catSymtabVA)
	if cfg.symtabAddrOverride != 0 {
		symtabAddr = cfg.symtabAddrOverride
	}
	strsz := uint64(len(strtab))
	if cfg.strsz != nil {
		strsz = *cfg.strsz
	}
	syment := uint64(elf64DynamicSymbolSize)
	if cfg.syment != nil {
		syment = *cfg.syment
	}
	entries := []dynEnt{
		{dynTagStrTab, strtabAddr},
		{dynTagStrSz, strsz},
		{dynTagSymTab, symtabAddr},
		{dynTagSymEnt, syment},
	}
	var hashBytes []byte
	if cfg.useHash {
		nchain := cfg.hashNchainOverride
		if nchain == 0 {
			nchain = uint32(len(cfg.syms))
		}
		hashBytes = buildELFHash(bo, 3, nchain)
		entries = append(entries, dynEnt{dynTagHash, catHashVA})
	}
	if cfg.gnu != nil {
		entries = append(entries, dynEnt{dynTagGNUHash, catGNUHashVA})
	}
	if cfg.versym != nil {
		entries = append(entries, dynEnt{dynTagVersym, catVersymVA})
	}
	if cfg.verneed != nil {
		entries = append(entries, dynEnt{dynTagVerNeed, catVerneedVA})
		num := uint64(len(cfg.verneed))
		if cfg.verneedNum != nil {
			num = *cfg.verneedNum
		}
		entries = append(entries, dynEnt{dynTagVerNeedNum, num})
	}
	entries = append(entries, cfg.extraDynamic...)
	segs := []segmentSpec{{vaddr: catStrtabVA, data: strtab, flags: 4}}
	if cfg.versym != nil {
		segs = append(segs, segmentSpec{vaddr: catVersymVA,
			data: buildVersymBytes(bo, cfg.versym), flags: 4})
	}
	if cfg.verneed != nil {
		segs = append(segs, segmentSpec{vaddr: catVerneedVA,
			data: buildVerneedBytes(bo, cfg.verneed, nameOffsets), flags: 4})
	}
	if cfg.useHash {
		segs = append(segs, segmentSpec{vaddr: catHashVA, data: hashBytes,
			flags: 4})
	}
	if cfg.gnu != nil {
		segs = append(segs, segmentSpec{vaddr: catGNUHashVA,
			data: buildGNUHashBytes(bo, cfg.gnu), flags: 4})
	}
	symBytes := buildSymbolBytes(bo, cfg.syms, nameOffsets)
	if cfg.symtabSplit > 0 {
		// Map the symbol table across two adjacent PT_LOAD segments.
		segs = append(segs,
			segmentSpec{vaddr: catSymtabVA,
				data: symBytes[:cfg.symtabSplit], flags: 4},
			segmentSpec{vaddr: catSymtabVA + uint64(cfg.symtabSplit),
				data: symBytes[cfg.symtabSplit:], flags: 4})
	} else {
		segs = append(segs, segmentSpec{vaddr: catSymtabVA, data: symBytes,
			flags: 4})
	}
	segs = append(segs, segmentSpec{typ: DynamicLinkingSegment,
		vaddr: catDynamicVA, data: buildDynamicBytes(bo, entries), flags: 4})
	segs = append(segs, cfg.extraSegments...)
	return assembleImage(bo, segs)
}

// Parses a synthetic image and reads its dynamic imports, failing the test
// on any error.
func parseAndCatalog(t *testing.T, raw []byte) (*ELF64File, []DynamicImport) {
	t.Helper()
	f, e := ParseELF64File(raw)
	if e != nil {
		t.Fatalf("Failed parsing the synthetic image: %s", e)
	}
	imports, e := ReadDynamicImports(f)
	if e != nil {
		t.Fatalf("ReadDynamicImports failed: %s", e)
	}
	return f, imports
}

// Checks that reading the image fails with an error containing the given
// substring.
func expectError(t *testing.T, raw []byte, substring string) {
	t.Helper()
	f, e := ParseELF64File(raw)
	if e != nil {
		t.Fatalf("Failed parsing the synthetic image: %s", e)
	}
	_, e = ReadDynamicImports(f)
	if e == nil {
		t.Fatalf("Expected an error containing %q, got nil", substring)
	}
	if !strings.Contains(e.Error(), substring) {
		t.Fatalf("Expected an error containing %q, got: %s", substring, e)
	}
	t.Logf("Got expected error: %s", e)
}

// Compares the actual and expected import records.
func checkImports(t *testing.T, got, want []DynamicImport) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("Expected %d imports, got %d: %+v", len(want), len(got),
			got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("Import %d: expected %+v, got %+v", i, want[i], got[i])
		}
	}
}

func TestDynamicCatalog(t *testing.T) {
	t.Run("LittleEndianHashAndVersions", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo: binary.LittleEndian,
			syms: []dynSymSpec{
				{}, // the null symbol
				{name: "local_func", binding: 0, typ: 2, shndx: 3},
				{name: "printf", binding: 1, typ: 2},
				{name: "puts", binding: 1, typ: 2},
				// A second, distinct import with the same name.
				{name: "printf", binding: 2, typ: 2},
				{name: "data_obj", binding: 1, typ: 1, shndx: 2},
				{}, // an undefined symbol with no name
			},
			versym: []uint16{0, 1, 2, 0x8003, 1, 1, 1},
			verneed: []verneedSpec{
				{file: "libfoo.so.6", aux: []vernauxSpec{
					{other: 2, name: "FOO_2.0"},
					{other: 4, name: "FOO_4.0"},
				}},
				{file: "libbar.so.1", aux: []vernauxSpec{
					{other: 3, name: "BAR_3.1"},
				}},
			},
			useHash: true,
			// A repeated single-value tag with an identical value must
			// be accepted.
			extraDynamic: []dynEnt{{dynTagStrTab, catStrtabVA}},
		})
		f, e := ParseELF64File(raw)
		if e != nil {
			t.Fatalf("Failed parsing the synthetic image: %s", e)
		}
		if len(f.Sections) != 0 {
			t.Fatalf("The synthetic image should have no sections, got %d",
				len(f.Sections))
		}
		rawBefore := append([]byte(nil), raw...)
		segmentsBefore := append([]ELF64ProgramHeader(nil), f.Segments...)
		imports, e := ReadDynamicImports(f)
		if e != nil {
			t.Fatalf("ReadDynamicImports failed: %s", e)
		}
		want := []DynamicImport{
			{Index: 2, Name: "printf", Binding: 1, Type: 2, RawVersym: 2,
				VersionIndex: 2, VersionName: "FOO_2.0",
				FileName: "libfoo.so.6"},
			{Index: 3, Name: "puts", Binding: 1, Type: 2, RawVersym: 0x8003,
				VersionIndex: 3, VersionName: "BAR_3.1",
				FileName: "libbar.so.1"},
			{Index: 4, Name: "printf", Binding: 2, Type: 2, RawVersym: 1,
				VersionIndex: 1},
		}
		checkImports(t, imports, want)
		// The file's state must be unchanged by the read.
		if !bytes.Equal(f.Raw, rawBefore) {
			t.Errorf("ReadDynamicImports modified the Raw buffer")
		}
		if len(f.Segments) != len(segmentsBefore) {
			t.Errorf("ReadDynamicImports changed the segment count")
		} else {
			for i := range f.Segments {
				if f.Segments[i] != segmentsBefore[i] {
					t.Errorf("ReadDynamicImports modified segment %d", i)
				}
			}
		}
		if len(f.Sections) != 0 {
			t.Errorf("ReadDynamicImports changed the section list")
		}
		// The records must not borrow the mutable input bytes: corrupting
		// the buffer must not affect them.
		for i := range raw {
			raw[i] ^= 0xff
		}
		checkImports(t, imports, want)
	})

	t.Run("BigEndianGNUHash", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo: binary.BigEndian,
			syms: []dynSymSpec{
				{},
				// Below symoffset, but still in the symbol table.
				{name: "early_bind", binding: 0, typ: 0},
				{name: "read", binding: 1, typ: 2},
				{name: "write", binding: 1, typ: 2},
				{name: "open64", binding: 1, typ: 2},
				{name: "local_init", binding: 0, typ: 2, shndx: 1},
			},
			gnu: &gnuHashSpec{
				symoffset: 2,
				// Bucket 0 is empty; buckets 1 and 2 share the chain
				// array, each ending at a terminator (low bit set).
				buckets: []uint32{0, 2, 4},
				chain:   []uint32{0x20, 0x41, 0x22, 0x43},
			},
		})
		_, imports := parseAndCatalog(t, raw)
		want := []DynamicImport{
			{Index: 1, Name: "early_bind", Binding: 0, Type: 0,
				RawVersym: 1, VersionIndex: 1},
			{Index: 2, Name: "read", Binding: 1, Type: 2, RawVersym: 1,
				VersionIndex: 1},
			{Index: 3, Name: "write", Binding: 1, Type: 2, RawVersym: 1,
				VersionIndex: 1},
			{Index: 4, Name: "open64", Binding: 1, Type: 2, RawVersym: 1,
				VersionIndex: 1},
		}
		checkImports(t, imports, want)
	})

	t.Run("ConsistentHashPair", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo: binary.LittleEndian,
			syms: []dynSymSpec{
				{},
				{name: "alpha", binding: 1, typ: 2},
				{name: "beta", binding: 1, typ: 2},
				{name: "gamma", binding: 1, typ: 2, shndx: 5},
			},
			versym: []uint16{0, 2, 0, 1},
			verneed: []verneedSpec{
				{file: "libz.so.1", aux: []vernauxSpec{
					{other: 2, name: "ZLIB_1.2"},
				}},
			},
			useHash: true, // nchain defaults to the symbol count, 4
			gnu: &gnuHashSpec{
				symoffset: 1,
				buckets:   []uint32{1},
				chain:     []uint32{0x10, 0x20, 0x31},
			},
		})
		_, imports := parseAndCatalog(t, raw)
		want := []DynamicImport{
			{Index: 1, Name: "alpha", Binding: 1, Type: 2, RawVersym: 2,
				VersionIndex: 2, VersionName: "ZLIB_1.2",
				FileName: "libz.so.1"},
			{Index: 2, Name: "beta", Binding: 1, Type: 2, RawVersym: 0,
				VersionIndex: 0},
		}
		checkImports(t, imports, want)
	})

	t.Run("HashCountMismatch", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:                 binary.LittleEndian,
			syms:               []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			useHash:            true,
			hashNchainOverride: 5,
			gnu: &gnuHashSpec{
				symoffset: 1,
				buckets:   []uint32{1},
				chain:     []uint32{0x31},
			},
		})
		expectError(t, raw, "disagree")
	})

	t.Run("MissingHashTablesUnsupported", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:   binary.LittleEndian,
			syms: []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
		})
		expectError(t, raw, "Unsupported")
	})

	t.Run("AdjacentSegmentSpan", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo: binary.LittleEndian,
			syms: []dynSymSpec{
				{},
				{name: "spanner", binding: 1, typ: 2},
				{name: "across", binding: 2, typ: 1},
			},
			versym: []uint16{0, 2, 2},
			verneed: []verneedSpec{
				{file: "libspan.so", aux: []vernauxSpec{
					{other: 2, name: "SPAN_1"},
				}},
			},
			useHash: true,
			// Split the symbol table in the middle of the second
			// symbol's bytes; the read must be reassembled from the
			// two adjacent mappings.
			symtabSplit: 30,
		})
		_, imports := parseAndCatalog(t, raw)
		want := []DynamicImport{
			{Index: 1, Name: "spanner", Binding: 1, Type: 2, RawVersym: 2,
				VersionIndex: 2, VersionName: "SPAN_1",
				FileName: "libspan.so"},
			{Index: 2, Name: "across", Binding: 2, Type: 1, RawVersym: 2,
				VersionIndex: 2, VersionName: "SPAN_1",
				FileName: "libspan.so"},
		}
		checkImports(t, imports, want)
	})

	t.Run("ZeroTailAddressRejected", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:                 binary.LittleEndian,
			syms:               []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			useHash:            true,
			strtabAddrOverride: 0x420040,
			extraSegments: []segmentSpec{
				// 32 file-backed bytes followed by a zero-filled
				// tail; DT_STRTAB points into the tail.
				{vaddr: 0x420000, data: make([]byte, 32), memsz: 0x1000,
					flags: 4},
			},
		})
		expectError(t, raw, "zero-filled")
	})

	t.Run("UnmappedAddressRejected", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:                 binary.LittleEndian,
			syms:               []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			useHash:            true,
			symtabAddrOverride: 0x500000,
		})
		f, e := ParseELF64File(raw)
		if e != nil {
			t.Fatalf("Failed parsing the synthetic image: %s", e)
		}
		rawBefore := append([]byte(nil), raw...)
		segmentsBefore := append([]ELF64ProgramHeader(nil), f.Segments...)
		_, e = ReadDynamicImports(f)
		if e == nil || !strings.Contains(e.Error(), "not backed") {
			t.Fatalf("Expected a 'not backed' error, got: %v", e)
		}
		t.Logf("Got expected error: %s", e)
		// Errors must leave the file's state untouched as well.
		if !bytes.Equal(f.Raw, rawBefore) {
			t.Errorf("The Raw buffer changed after the failed read")
		}
		if len(f.Segments) != len(segmentsBefore) {
			t.Errorf("The segment list changed after the failed read")
		} else {
			for i := range f.Segments {
				if f.Segments[i] != segmentsBefore[i] {
					t.Errorf("Segment %d changed after the failed read", i)
				}
			}
		}
	})

	t.Run("ConflictingMappingsRejected", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:      binary.LittleEndian,
			syms:    []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			useHash: true,
			extraSegments: []segmentSpec{
				// Covers the symbol table's address a second time,
				// from a different file offset.
				{vaddr: catSymtabVA, data: make([]byte, 64), flags: 4},
			},
		})
		expectError(t, raw, "multiple PT_LOAD")
	})

	t.Run("UnterminatedStringRejected", func(t *testing.T) {
		// The table is "\0foo\0bar\0" (9 bytes); claiming a size of 8
		// leaves "bar" without its terminator inside DT_STRSZ.
		strsz := uint64(8)
		raw := buildCatalogImage(catalogImageConfig{
			bo: binary.LittleEndian,
			syms: []dynSymSpec{
				{},
				{name: "foo", binding: 1, typ: 2},
				{name: "bar", binding: 1, typ: 2},
			},
			useHash: true,
			strsz:   &strsz,
		})
		expectError(t, raw, "Unterminated")
	})

	t.Run("BadSymbolEntrySizeRejected", func(t *testing.T) {
		syment := uint64(16)
		raw := buildCatalogImage(catalogImageConfig{
			bo:      binary.LittleEndian,
			syms:    []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			useHash: true,
			syment:  &syment,
		})
		expectError(t, raw, "DT_SYMENT")
	})

	t.Run("ConflictingDuplicateTagRejected", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:      binary.LittleEndian,
			syms:    []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			useHash: true,
			// A second DT_SYMTAB with a different value.
			extraDynamic: []dynEnt{{dynTagSymTab, catSymtabVA + 0x100}},
		})
		expectError(t, raw, "appears twice")
	})

	t.Run("VersionIndexConflictRejected", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:     binary.LittleEndian,
			syms:   []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			versym: []uint16{0, 2},
			verneed: []verneedSpec{
				{file: "liba.so", aux: []vernauxSpec{
					{other: 2, name: "VER_A"},
				}},
				{file: "libb.so", aux: []vernauxSpec{
					{other: 2, name: "VER_B"},
				}},
			},
			useHash: true,
		})
		expectError(t, raw, "conflicting")
	})

	t.Run("BrokenVerneedChainRejected", func(t *testing.T) {
		verneedNum := uint64(3)
		raw := buildCatalogImage(catalogImageConfig{
			bo:     binary.LittleEndian,
			syms:   []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			versym: []uint16{0, 2},
			verneed: []verneedSpec{
				{file: "liba.so", aux: []vernauxSpec{
					{other: 2, name: "VER_A"},
				}},
				{file: "libb.so", aux: []vernauxSpec{
					{other: 3, name: "VER_B"},
				}},
			},
			// The chain holds 2 records but the count claims 3.
			verneedNum: &verneedNum,
			useHash:    true,
		})
		expectError(t, raw, "before the expected")
	})

	t.Run("UnresolvableVersionRejected", func(t *testing.T) {
		raw := buildCatalogImage(catalogImageConfig{
			bo:     binary.LittleEndian,
			syms:   []dynSymSpec{{}, {name: "alpha", binding: 1, typ: 2}},
			versym: []uint16{0, 2},
			verneed: []verneedSpec{
				{file: "liba.so", aux: []vernauxSpec{
					{other: 3, name: "VER_A"},
				}},
			},
			useHash: true,
		})
		expectError(t, raw, "no DT_VERNEED definition")
	})

	t.Run("DynamicSegmentCountProblems", func(t *testing.T) {
		// No PT_DYNAMIC at all.
		raw := assembleImage(binary.LittleEndian, []segmentSpec{
			{vaddr: 0x400000, data: []byte{1, 2, 3, 4}, flags: 4},
		})
		expectError(t, raw, "No PT_DYNAMIC")
		// More than one PT_DYNAMIC.
		dynamic := buildDynamicBytes(binary.LittleEndian, nil)
		raw = assembleImage(binary.LittleEndian, []segmentSpec{
			{typ: DynamicLinkingSegment, vaddr: 0x400000, data: dynamic,
				flags: 4},
			{typ: DynamicLinkingSegment, vaddr: 0x401000, data: dynamic,
				flags: 4},
		})
		expectError(t, raw, "Multiple PT_DYNAMIC")
	})
}
