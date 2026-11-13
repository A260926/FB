package elf_reader

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// The base virtual address and the file offset of the dynamic table used by
// the synthetic images in this file.
const (
	synthImageBase  = 0x400000
	synthDynamicOfs = 0x100
)

// Serializes the given values using binary.Write, panicking on error.
func mustBytes(order binary.ByteOrder, values ...interface{}) []byte {
	var buffer bytes.Buffer
	for _, value := range values {
		if e := binary.Write(&buffer, order, value); e != nil {
			panic(e)
		}
	}
	return buffer.Bytes()
}

// Builds a string table (starting with an empty string) and returns its
// contents along with the offset of each string.
func synthStringTable(names ...string) ([]byte, map[string]uint32) {
	data := []byte{0}
	offsets := make(map[string]uint32)
	for _, name := range names {
		if name == "" {
			continue
		}
		if _, ok := offsets[name]; ok {
			continue
		}
		offsets[name] = uint32(len(data))
		data = append(data, name...)
		data = append(data, 0)
	}
	return data, offsets
}

// Serializes a symbol table.
func synthSymbols(order binary.ByteOrder, symbols []ELF64Symbol) []byte {
	return mustBytes(order, symbols)
}

// Builds a minimal DT_HASH table with the given nchain. The buckets and
// chains are all zero; only nchain matters for counting symbols.
func synthElfHash(order binary.ByteOrder, nchain uint32) []byte {
	out := mustBytes(order, uint32(1), nchain, uint32(0))
	return append(out, make([]byte, int(nchain)*4)...)
}

// Builds a DT_GNU_HASH table with a single-word bloom filter.
func synthGnuHash(order binary.ByteOrder, symbolOffset uint32,
	buckets []uint32, chain []uint32) []byte {
	out := mustBytes(order, uint32(len(buckets)), symbolOffset, uint32(1),
		uint32(0), uint64(0))
	out = append(out, mustBytes(order, buckets)...)
	return append(out, mustBytes(order, chain)...)
}

// Describes one version need entry for synthVerneed.
type verneedSpec struct {
	file     string
	versions []versymSpec
}

// Describes one version auxiliary entry for synthVerneed.
type versymSpec struct {
	name  string
	index uint16
}

// Builds a DT_VERNEED chain. Auxiliary entries immediately follow their
// verneed entry, and all offsets are relative, as in real files.
func synthVerneed(order binary.ByteOrder, offsets map[string]uint32,
	needs []verneedSpec) []byte {
	var out []byte
	for i, need := range needs {
		next := uint32(0)
		if i < len(needs)-1 {
			next = uint32(16 + len(need.versions)*16)
		}
		out = append(out, mustBytes(order, uint16(1),
			uint16(len(need.versions)), offsets[need.file], uint32(16),
			next)...)
		for j, version := range need.versions {
			auxNext := uint32(0)
			if j < len(need.versions)-1 {
				auxNext = 16
			}
			out = append(out, mustBytes(order, uint32(0), uint16(0),
				version.index, offsets[version.name], auxNext)...)
		}
	}
	return out
}

// A named blob of data to place in a synthetic image.
type imagePart struct {
	name  string
	align uint64
	data  []byte
}

// Builds a section-less 64-bit ELF image: an ELF header, two or three
// program headers (PT_LOAD segments covering the whole file, plus
// PT_DYNAMIC), the dynamic table, and the given parts. If split is nonzero,
// the file is covered by two adjacent PT_LOAD segments split at the given
// file offset. makeDynamics receives the virtual addresses of the named
// parts and must return exactly dynamicCount entries.
func buildDynamicImage(t *testing.T, order binary.ByteOrder, dynamicCount int,
	split uint64, parts []imagePart,
	makeDynamics func(addrs map[string]uint64) []ELF64DynamicEntry) ([]byte,
	map[string]uint64) {
	t.Helper()
	phdrCount := 2
	if split != 0 {
		phdrCount = 3
	}
	offsets := make([]uint64, len(parts))
	addrs := make(map[string]uint64)
	total := uint64(synthDynamicOfs + dynamicCount*16)
	for i := range parts {
		align := parts[i].align
		if align < 1 {
			align = 1
		}
		total = (total + align - 1) &^ (align - 1)
		offsets[i] = total
		if parts[i].name != "" {
			addrs[parts[i].name] = synthImageBase + total
		}
		total += uint64(len(parts[i].data))
	}
	if (split != 0) && ((split <= synthDynamicOfs) || (split >= total)) {
		t.Fatalf("Invalid split offset 0x%x for an image of size 0x%x",
			split, total)
	}
	dynamics := makeDynamics(addrs)
	if len(dynamics) != dynamicCount {
		t.Fatalf("makeDynamics returned %d entries, expected %d",
			len(dynamics), dynamicCount)
	}
	endianness := uint8(1)
	signature := uint32(0x464c457f)
	if order == binary.BigEndian {
		endianness = 2
		signature = 0x7f454c46
	}
	header := ELF64Header{
		Signature:              signature,
		Class:                  2,
		Endianness:             endianness,
		Version:                1,
		Version2:               1,
		Type:                   ELFTypeShared,
		Machine:                MachineTypeAMD64,
		HeaderSize:             64,
		ProgramHeaderOffset:    64,
		ProgramHeaderEntrySize: 56,
		ProgramHeaderEntries:   uint16(phdrCount),
	}
	phdrs := make([]ELF64ProgramHeader, 0, phdrCount)
	if split == 0 {
		phdrs = append(phdrs, ELF64ProgramHeader{
			Type: LoadableSegment, Flags: 5, FileOffset: 0,
			VirtualAddress: synthImageBase,
			// PhysicalAddress is unused by the reader.
			PhysicalAddress: synthImageBase,
			FileSize:        total, MemorySize: total, Align: 0x1000,
		})
	} else {
		phdrs = append(phdrs, ELF64ProgramHeader{
			Type: LoadableSegment, Flags: 5, FileOffset: 0,
			VirtualAddress:  synthImageBase,
			PhysicalAddress: synthImageBase,
			FileSize:        split, MemorySize: split, Align: 0x1000,
		}, ELF64ProgramHeader{
			Type: LoadableSegment, Flags: 5, FileOffset: split,
			VirtualAddress:  synthImageBase + split,
			PhysicalAddress: synthImageBase + split,
			FileSize:        total - split, MemorySize: total - split,
			Align: 0x1000,
		})
	}
	phdrs = append(phdrs, ELF64ProgramHeader{
		Type: DynamicLinkingSegment, Flags: 4, FileOffset: synthDynamicOfs,
		VirtualAddress:  synthImageBase + synthDynamicOfs,
		PhysicalAddress: synthImageBase + synthDynamicOfs,
		FileSize:        uint64(dynamicCount) * 16,
		MemorySize:      uint64(dynamicCount) * 16,
		Align:           8,
	})
	raw := make([]byte, total)
	copy(raw, mustBytes(order, header))
	copy(raw[64:], mustBytes(order, phdrs))
	copy(raw[synthDynamicOfs:], mustBytes(order, dynamics))
	for i := range parts {
		copy(raw[offsets[i]:], parts[i].data)
	}
	return raw, addrs
}

// Overwrites the value of the first dynamic entry with the given tag in a
// synthetic image. Returns false if the tag wasn't found before DT_NULL.
func setDynamicValue(raw []byte, order binary.ByteOrder, tag int64,
	value uint64) bool {
	for ofs := synthDynamicOfs; ofs+16 <= len(raw); ofs += 16 {
		current := int64(order.Uint64(raw[ofs:]))
		if current == tag {
			order.PutUint64(raw[ofs+8:], value)
			return true
		}
		if current == 0 {
			return false
		}
	}
	return false
}

// Parses the image and runs ReadDynamicImports on it.
func parseAndReadImports(t *testing.T, raw []byte) (*ELF64File,
	[]DynamicImport, error) {
	t.Helper()
	f, e := ParseELF64File(raw)
	if e != nil {
		t.Fatalf("Failed parsing the synthetic image: %s", e)
	}
	imports, e := ReadDynamicImports(f)
	return f, imports, e
}

// Expects ReadDynamicImports to fail on the given image.
func expectImportError(t *testing.T, raw []byte, context string) {
	t.Helper()
	_, _, e := parseAndReadImports(t, raw)
	if e == nil {
		t.Fatalf("%s: expected an error but got none", context)
	}
	t.Logf("%s: got expected error: %s", context, e)
}

// Compares two import lists.
func checkImports(t *testing.T, context string, got []DynamicImport,
	want []DynamicImport) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("%s: expected %d imports, got %d: %+v", context,
			len(want), len(got), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("%s: import %d:\n  got: %+v\n want: %+v", context,
				i, got[i], want[i])
		}
	}
}

// Builds an image whose symbol table straddles two adjacent PT_LOAD
// segments: the file is split at offset 0x1a0, and the symbol table covers
// [0x178, 0x1c0) in the file. The string table and hash table live in the
// second segment.
func buildSplitCatalogImage(t *testing.T) ([]byte, map[string]uint64) {
	t.Helper()
	order := binary.LittleEndian
	strtab, ofs := synthStringTable("cross_a", "cross_b")
	symbols := []ELF64Symbol{
		{},
		{Name: ofs["cross_a"], Info: 0x12},
		{Name: ofs["cross_b"], Info: 0x12},
	}
	parts := []imagePart{
		{name: "hash", align: 8, data: synthElfHash(order, 3)},
		{name: "dynsym", align: 8, data: synthSymbols(order, symbols)},
		{name: "strtab", align: 1, data: strtab},
	}
	makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
		return []ELF64DynamicEntry{
			{Tag: ELF64DynamicTag(dynamicTagStrtab),
				Value: addrs["strtab"]},
			{Tag: ELF64DynamicTag(dynamicTagStrsz),
				Value: uint64(len(strtab))},
			{Tag: ELF64DynamicTag(dynamicTagSymtab),
				Value: addrs["dynsym"]},
			{Tag: ELF64DynamicTag(dynamicTagSyment), Value: 24},
			{Tag: ELF64DynamicTag(dynamicTagHash),
				Value: addrs["hash"]},
			{Tag: 0, Value: 0},
		}
	}
	return buildDynamicImage(t, order, 6, 0x1a0, parts, makeDynamics)
}

func TestDynamicCatalog(t *testing.T) {
	t.Run("LittleEndianHashAndVersions", func(t *testing.T) {
		order := binary.LittleEndian
		strtab, ofs := synthStringTable("local_defined", "printf",
			"malloc", "pthread_create", "stderr", "libfoo.so",
			"libbar.so", "FOO_1", "FOO_2", "BAR_1")
		symbols := []ELF64Symbol{
			{},
			{Name: ofs["local_defined"], Info: 0, SectionIndex: 5},
			{Name: ofs["printf"], Info: 0x12},
			{Name: ofs["malloc"], Info: 0x12},
			{Name: ofs["pthread_create"], Info: 0x22},
			// A second import with the same name must be kept.
			{Name: ofs["printf"], Info: 0x12},
			{Name: ofs["stderr"], Info: 0x11},
		}
		versym := mustBytes(order, []uint16{0, 0, 2, 3, 0x8004, 1, 0})
		verneed := synthVerneed(order, ofs, []verneedSpec{
			{file: "libfoo.so", versions: []versymSpec{
				{name: "FOO_1", index: 2},
				{name: "FOO_2", index: 3},
			}},
			{file: "libbar.so", versions: []versymSpec{
				{name: "BAR_1", index: 4},
			}},
		})
		parts := []imagePart{
			{name: "dynsym", align: 8,
				data: synthSymbols(order, symbols)},
			{name: "strtab", align: 1, data: strtab},
			{name: "hash", align: 4, data: synthElfHash(order, 7)},
			{name: "versym", align: 2, data: versym},
			{name: "verneed", align: 4, data: verneed},
		}
		makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
			return []ELF64DynamicEntry{
				{Tag: ELF64DynamicTag(dynamicTagStrtab),
					Value: addrs["strtab"]},
				{Tag: ELF64DynamicTag(dynamicTagStrsz),
					Value: uint64(len(strtab))},
				{Tag: ELF64DynamicTag(dynamicTagSymtab),
					Value: addrs["dynsym"]},
				{Tag: ELF64DynamicTag(dynamicTagSyment), Value: 24},
				{Tag: ELF64DynamicTag(dynamicTagHash),
					Value: addrs["hash"]},
				{Tag: ELF64DynamicTag(dynamicTagVersym),
					Value: addrs["versym"]},
				{Tag: ELF64DynamicTag(dynamicTagVerneed),
					Value: addrs["verneed"]},
				{Tag: ELF64DynamicTag(dynamicTagVerneednum),
					Value: 2},
				{Tag: 0, Value: 0},
			}
		}
		raw, _ := buildDynamicImage(t, order, 9, 0, parts, makeDynamics)
		rawBefore := make([]byte, len(raw))
		copy(rawBefore, raw)
		f, imports, e := parseAndReadImports(t, raw)
		if e != nil {
			t.Fatalf("Failed reading dynamic imports: %s", e)
		}
		want := []DynamicImport{
			{SymbolIndex: 2, Name: "printf", Binding: 1,
				SymbolType: 2, RawVersion: 2, VersionIndex: 2,
				VersionName: "FOO_1", DependencyFile: "libfoo.so"},
			{SymbolIndex: 3, Name: "malloc", Binding: 1,
				SymbolType: 2, RawVersion: 3, VersionIndex: 3,
				VersionName: "FOO_2", DependencyFile: "libfoo.so"},
			{SymbolIndex: 4, Name: "pthread_create", Binding: 2,
				SymbolType: 2, RawVersion: 0x8004,
				VersionIndex: 4, VersionName: "BAR_1",
				DependencyFile: "libbar.so"},
			{SymbolIndex: 5, Name: "printf", Binding: 1,
				SymbolType: 2, RawVersion: 1, VersionIndex: 1},
			{SymbolIndex: 6, Name: "stderr", Binding: 1,
				SymbolType: 1, RawVersion: 0, VersionIndex: 0},
		}
		checkImports(t, "little-endian versioned image", imports, want)
		// The file's parsed state must not change, on success or
		// failure.
		if !bytes.Equal(f.Raw, rawBefore) {
			t.Errorf("ReadDynamicImports modified the raw file " +
				"contents")
		}
		if len(f.Sections) != 0 {
			t.Errorf("The section list changed: %d entries",
				len(f.Sections))
		}
		if len(f.Segments) != 2 {
			t.Errorf("The segment list changed: %d entries",
				len(f.Segments))
		}
		// The returned records must not borrow the input bytes.
		printfOffset := bytes.Index(f.Raw, []byte("printf"))
		if printfOffset < 0 {
			t.Fatalf("Couldn't find \"printf\" in the image")
		}
		f.Raw[printfOffset] = 'X'
		if imports[0].Name != "printf" {
			t.Errorf("The import records borrowed mutable input "+
				"bytes: name is now %q", imports[0].Name)
		}
	})

	t.Run("BigEndianGnuHash", func(t *testing.T) {
		order := binary.BigEndian
		strtab, ofs := synthStringTable("local_fn", "fopen", "fclose",
			"errno", "libssl.so", "SSL_3")
		symbols := []ELF64Symbol{
			{},
			{Name: ofs["local_fn"], Info: 0x12, SectionIndex: 3},
			{Name: ofs["fopen"], Info: 0x12},
			{Name: ofs["fclose"], Info: 0x12},
			{Name: ofs["errno"], Info: 0x11},
		}
		// symoffset is 2, bucket 0 is empty, and buckets 1 and 2
		// share the tail of the chain.
		gnuHash := synthGnuHash(order, 2, []uint32{0, 2, 3},
			[]uint32{0, 2, 3})
		versym := mustBytes(order, []uint16{0, 0, 2, 2, 0})
		verneed := synthVerneed(order, ofs, []verneedSpec{
			{file: "libssl.so", versions: []versymSpec{
				{name: "SSL_3", index: 2},
			}},
		})
		parts := []imagePart{
			{name: "dynsym", align: 8,
				data: synthSymbols(order, symbols)},
			{name: "strtab", align: 1, data: strtab},
			{name: "gnuhash", align: 8, data: gnuHash},
			{name: "versym", align: 2, data: versym},
			{name: "verneed", align: 4, data: verneed},
		}
		makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
			return []ELF64DynamicEntry{
				{Tag: ELF64DynamicTag(dynamicTagStrtab),
					Value: addrs["strtab"]},
				{Tag: ELF64DynamicTag(dynamicTagStrsz),
					Value: uint64(len(strtab))},
				{Tag: ELF64DynamicTag(dynamicTagSymtab),
					Value: addrs["dynsym"]},
				{Tag: ELF64DynamicTag(dynamicTagSyment), Value: 24},
				{Tag: ELF64DynamicTag(dynamicTagGnuHash),
					Value: addrs["gnuhash"]},
				{Tag: ELF64DynamicTag(dynamicTagVersym),
					Value: addrs["versym"]},
				{Tag: ELF64DynamicTag(dynamicTagVerneed),
					Value: addrs["verneed"]},
				{Tag: ELF64DynamicTag(dynamicTagVerneednum),
					Value: 1},
				{Tag: 0, Value: 0},
			}
		}
		raw, _ := buildDynamicImage(t, order, 9, 0, parts, makeDynamics)
		f, imports, e := parseAndReadImports(t, raw)
		if e != nil {
			t.Fatalf("Failed reading big-endian imports: %s", e)
		}
		if f.Endianness != binary.BigEndian {
			t.Errorf("The file wasn't parsed as big-endian")
		}
		want := []DynamicImport{
			{SymbolIndex: 2, Name: "fopen", Binding: 1,
				SymbolType: 2, RawVersion: 2, VersionIndex: 2,
				VersionName:    "SSL_3",
				DependencyFile: "libssl.so"},
			{SymbolIndex: 3, Name: "fclose", Binding: 1,
				SymbolType: 2, RawVersion: 2, VersionIndex: 2,
				VersionName:    "SSL_3",
				DependencyFile: "libssl.so"},
			{SymbolIndex: 4, Name: "errno", Binding: 1,
				SymbolType: 1, RawVersion: 0, VersionIndex: 0},
		}
		checkImports(t, "big-endian GNU hash image", imports, want)
	})

	t.Run("BothHashTypesPresent", func(t *testing.T) {
		order := binary.LittleEndian
		strtab, ofs := synthStringTable("alpha", "beta")
		symbols := []ELF64Symbol{
			{},
			{Name: ofs["alpha"], Info: 0x12},
			{Name: ofs["beta"], Info: 0x10},
		}
		// Both hash types imply 3 symbols.
		gnuHash := synthGnuHash(order, 1, []uint32{1}, []uint32{0, 1})
		parts := []imagePart{
			{name: "dynsym", align: 8,
				data: synthSymbols(order, symbols)},
			{name: "strtab", align: 1, data: strtab},
			{name: "hash", align: 4, data: synthElfHash(order, 3)},
			{name: "gnuhash", align: 8, data: gnuHash},
		}
		makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
			return []ELF64DynamicEntry{
				{Tag: ELF64DynamicTag(dynamicTagStrtab),
					Value: addrs["strtab"]},
				// A repeated single-valued tag with the same
				// value must be accepted.
				{Tag: ELF64DynamicTag(dynamicTagStrtab),
					Value: addrs["strtab"]},
				{Tag: ELF64DynamicTag(dynamicTagStrsz),
					Value: uint64(len(strtab))},
				{Tag: ELF64DynamicTag(dynamicTagSymtab),
					Value: addrs["dynsym"]},
				{Tag: ELF64DynamicTag(dynamicTagSyment), Value: 24},
				{Tag: ELF64DynamicTag(dynamicTagHash),
					Value: addrs["hash"]},
				{Tag: ELF64DynamicTag(dynamicTagGnuHash),
					Value: addrs["gnuhash"]},
				{Tag: 0, Value: 0},
			}
		}
		raw, _ := buildDynamicImage(t, order, 8, 0, parts, makeDynamics)
		_, imports, e := parseAndReadImports(t, raw)
		if e != nil {
			t.Fatalf("Failed reading imports with both hashes: %s", e)
		}
		// Without DT_VERSYM, the raw version and index default to 1
		// and the names stay empty.
		want := []DynamicImport{
			{SymbolIndex: 1, Name: "alpha", Binding: 1,
				SymbolType: 2, RawVersion: 1, VersionIndex: 1},
			{SymbolIndex: 2, Name: "beta", Binding: 1,
				SymbolType: 0, RawVersion: 1, VersionIndex: 1},
		}
		checkImports(t, "image with both hash types", imports, want)
	})

	t.Run("HashTableErrors", func(t *testing.T) {
		t.Run("CountMismatch", func(t *testing.T) {
			order := binary.LittleEndian
			strtab, ofs := synthStringTable("alpha", "beta")
			symbols := []ELF64Symbol{
				{},
				{Name: ofs["alpha"], Info: 0x12},
				{Name: ofs["beta"], Info: 0x12},
			}
			gnuHash := synthGnuHash(order, 1, []uint32{1},
				[]uint32{0, 1})
			parts := []imagePart{
				{name: "dynsym", align: 8,
					data: synthSymbols(order, symbols)},
				{name: "strtab", align: 1, data: strtab},
				// DT_HASH says 4 symbols, DT_GNU_HASH says 3.
				{name: "hash", align: 4,
					data: synthElfHash(order, 4)},
				{name: "gnuhash", align: 8, data: gnuHash},
			}
			makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
				return []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSymtab),
						Value: addrs["dynsym"]},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: ELF64DynamicTag(dynamicTagHash),
						Value: addrs["hash"]},
					{Tag: ELF64DynamicTag(dynamicTagGnuHash),
						Value: addrs["gnuhash"]},
					{Tag: 0, Value: 0},
				}
			}
			raw, _ := buildDynamicImage(t, order, 7, 0, parts,
				makeDynamics)
			expectImportError(t, raw, "DT_HASH/DT_GNU_HASH count "+
				"mismatch")
		})
		t.Run("InvalidBucketIndex", func(t *testing.T) {
			order := binary.LittleEndian
			strtab, ofs := synthStringTable("alpha", "beta")
			symbols := []ELF64Symbol{
				{},
				{Name: ofs["alpha"], Info: 0x12},
				{Name: ofs["beta"], Info: 0x12},
			}
			// The bucket refers to symbol 1, below symoffset 2.
			gnuHash := synthGnuHash(order, 2, []uint32{1},
				[]uint32{1})
			parts := []imagePart{
				{name: "dynsym", align: 8,
					data: synthSymbols(order, symbols)},
				{name: "strtab", align: 1, data: strtab},
				{name: "gnuhash", align: 8, data: gnuHash},
			}
			makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
				return []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSymtab),
						Value: addrs["dynsym"]},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: ELF64DynamicTag(dynamicTagGnuHash),
						Value: addrs["gnuhash"]},
					{Tag: 0, Value: 0},
				}
			}
			raw, _ := buildDynamicImage(t, order, 6, 0, parts,
				makeDynamics)
			expectImportError(t, raw, "GNU hash bucket below the "+
				"symbol offset")
		})
	})

	t.Run("CrossSegmentLayout", func(t *testing.T) {
		raw, addrs := buildSplitCatalogImage(t)
		if addrs["dynsym"] != synthImageBase+0x178 {
			t.Fatalf("The symbol table should start at 0x%x, got "+
				"0x%x", synthImageBase+0x178, addrs["dynsym"])
		}
		_, imports, e := parseAndReadImports(t, raw)
		if e != nil {
			t.Fatalf("Failed reading imports across load segments: "+
				"%s", e)
		}
		want := []DynamicImport{
			{SymbolIndex: 1, Name: "cross_a", Binding: 1,
				SymbolType: 2, RawVersion: 1, VersionIndex: 1},
			{SymbolIndex: 2, Name: "cross_b", Binding: 1,
				SymbolType: 2, RawVersion: 1, VersionIndex: 1},
		}
		checkImports(t, "cross-segment image", imports, want)
	})

	t.Run("AddressMappingErrors", func(t *testing.T) {
		t.Run("UnmappedHole", func(t *testing.T) {
			raw, _ := buildSplitCatalogImage(t)
			// Point the string table past every mapping.
			if !setDynamicValue(raw, binary.LittleEndian,
				dynamicTagStrtab,
				synthImageBase+uint64(len(raw))+0x1000) {
				t.Fatalf("Couldn't find the DT_STRTAB entry")
			}
			expectImportError(t, raw, "string table in an unmapped "+
				"hole")
		})
		t.Run("ZeroFilledTail", func(t *testing.T) {
			raw, _ := buildSplitCatalogImage(t)
			// Extend the first segment's memory size past its
			// file size, then point the string table into the
			// resulting zero-filled tail.
			binary.LittleEndian.PutUint64(raw[64+40:],
				uint64(len(raw))+0x100)
			if !setDynamicValue(raw, binary.LittleEndian,
				dynamicTagStrtab,
				synthImageBase+uint64(len(raw))+0x10) {
				t.Fatalf("Couldn't find the DT_STRTAB entry")
			}
			expectImportError(t, raw, "string table in a "+
				"zero-filled tail")
		})
		t.Run("ConflictingMappings", func(t *testing.T) {
			raw, _ := buildSplitCatalogImage(t)
			// Move the second load segment's virtual address so
			// that it overlaps the first one while keeping its
			// original (different) file contents.
			binary.LittleEndian.PutUint64(raw[64+56+16:],
				synthImageBase+0x180)
			expectImportError(t, raw, "overlapping segments with "+
				"different contents")
		})
	})

	t.Run("DynamicTableErrors", func(t *testing.T) {
		order := binary.LittleEndian
		strtab, ofs := synthStringTable("alpha", "beta")
		symbols := []ELF64Symbol{
			{},
			{Name: ofs["alpha"], Info: 0x12},
			{Name: ofs["beta"], Info: 0x12},
		}
		parts := []imagePart{
			{name: "dynsym", align: 8,
				data: synthSymbols(order, symbols)},
			{name: "strtab", align: 1, data: strtab},
			{name: "hash", align: 4, data: synthElfHash(order, 3)},
		}
		standard := func(addrs map[string]uint64) []ELF64DynamicEntry {
			return []ELF64DynamicEntry{
				{Tag: ELF64DynamicTag(dynamicTagStrtab),
					Value: addrs["strtab"]},
				{Tag: ELF64DynamicTag(dynamicTagStrsz),
					Value: uint64(len(strtab))},
				{Tag: ELF64DynamicTag(dynamicTagSymtab),
					Value: addrs["dynsym"]},
				{Tag: ELF64DynamicTag(dynamicTagSyment), Value: 24},
				{Tag: ELF64DynamicTag(dynamicTagHash),
					Value: addrs["hash"]},
				{Tag: 0, Value: 0},
			}
		}
		t.Run("NoDynamicSegment", func(t *testing.T) {
			raw, _ := buildDynamicImage(t, order, 6, 0, parts,
				standard)
			// Turn the PT_DYNAMIC program header into PT_NULL.
			binary.LittleEndian.PutUint32(raw[64+56:], 0)
			expectImportError(t, raw, "no PT_DYNAMIC segment")
		})
		t.Run("MultipleDynamicSegments", func(t *testing.T) {
			raw, _ := buildDynamicImage(t, order, 6, 0, parts,
				standard)
			// Turn the load segment into a second PT_DYNAMIC.
			binary.LittleEndian.PutUint32(raw[64:], 2)
			expectImportError(t, raw, "multiple PT_DYNAMIC segments")
		})
		t.Run("MissingNullEntry", func(t *testing.T) {
			noNull := func(addrs map[string]uint64) []ELF64DynamicEntry {
				return []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSymtab),
						Value: addrs["dynsym"]},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: ELF64DynamicTag(dynamicTagHash),
						Value: addrs["hash"]},
					{Tag: 12, Value: 0x1000},
				}
			}
			raw, _ := buildDynamicImage(t, order, 6, 0, parts,
				noNull)
			expectImportError(t, raw, "no DT_NULL entry")
		})
		t.Run("BadSymbolEntrySize", func(t *testing.T) {
			raw, _ := buildDynamicImage(t, order, 6, 0, parts,
				standard)
			if !setDynamicValue(raw, order, dynamicTagSyment, 25) {
				t.Fatalf("Couldn't find the DT_SYMENT entry")
			}
			expectImportError(t, raw, "bad DT_SYMENT")
		})
		t.Run("MissingSymtab", func(t *testing.T) {
			noSymtab := func(addrs map[string]uint64) []ELF64DynamicEntry {
				return []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: ELF64DynamicTag(dynamicTagHash),
						Value: addrs["hash"]},
					{Tag: 0, Value: 0},
				}
			}
			raw, _ := buildDynamicImage(t, order, 5, 0, parts,
				noSymtab)
			expectImportError(t, raw, "no DT_SYMTAB")
		})
		t.Run("MissingHash", func(t *testing.T) {
			noHash := func(addrs map[string]uint64) []ELF64DynamicEntry {
				return []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSymtab),
						Value: addrs["dynsym"]},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: 0, Value: 0},
				}
			}
			raw, _ := buildDynamicImage(t, order, 5, 0, parts,
				noHash)
			expectImportError(t, raw, "no hash table")
		})
		t.Run("ConflictingRepeatedTag", func(t *testing.T) {
			duplicate := func(addrs map[string]uint64) []ELF64DynamicEntry {
				return []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"] + 1},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSymtab),
						Value: addrs["dynsym"]},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: ELF64DynamicTag(dynamicTagHash),
						Value: addrs["hash"]},
					{Tag: 0, Value: 0},
				}
			}
			raw, _ := buildDynamicImage(t, order, 7, 0, parts,
				duplicate)
			expectImportError(t, raw, "conflicting DT_STRTAB values")
		})
	})

	t.Run("VersionErrors", func(t *testing.T) {
		order := binary.LittleEndian
		// Builds a three-symbol image with the given version data.
		build := func(versym []uint16, verneed []byte,
			verneedNum uint64, includeVerneednum bool,
			strtab []byte, ofs map[string]uint32) []byte {
			symbols := []ELF64Symbol{
				{},
				{Name: ofs["one"], Info: 0x12},
				{Name: ofs["two"], Info: 0x12},
			}
			parts := []imagePart{
				{name: "dynsym", align: 8,
					data: synthSymbols(order, symbols)},
				{name: "strtab", align: 1, data: strtab},
				{name: "hash", align: 4,
					data: synthElfHash(order, 3)},
				{name: "versym", align: 2,
					data: mustBytes(order, versym)},
				{name: "verneed", align: 4, data: verneed},
			}
			dynamicCount := 9
			if !includeVerneednum {
				dynamicCount = 8
			}
			makeDynamics := func(addrs map[string]uint64) []ELF64DynamicEntry {
				entries := []ELF64DynamicEntry{
					{Tag: ELF64DynamicTag(dynamicTagStrtab),
						Value: addrs["strtab"]},
					{Tag: ELF64DynamicTag(dynamicTagStrsz),
						Value: uint64(len(strtab))},
					{Tag: ELF64DynamicTag(dynamicTagSymtab),
						Value: addrs["dynsym"]},
					{Tag: ELF64DynamicTag(dynamicTagSyment),
						Value: 24},
					{Tag: ELF64DynamicTag(dynamicTagHash),
						Value: addrs["hash"]},
					{Tag: ELF64DynamicTag(dynamicTagVersym),
						Value: addrs["versym"]},
					{Tag: ELF64DynamicTag(dynamicTagVerneed),
						Value: addrs["verneed"]},
				}
				if includeVerneednum {
					entries = append(entries, ELF64DynamicEntry{
						Tag: ELF64DynamicTag(
							dynamicTagVerneednum),
						Value: verneedNum,
					})
				}
				return append(entries, ELF64DynamicEntry{Tag: 0,
					Value: 0})
			}
			raw, _ := buildDynamicImage(t, order, dynamicCount, 0,
				parts, makeDynamics)
			return raw
		}
		strtab, ofs := synthStringTable("one", "two", "libx.so",
			"liby.so", "VER_A", "VER_B")
		goodVerneed := func() []byte {
			return synthVerneed(order, ofs, []verneedSpec{
				{file: "libx.so", versions: []versymSpec{
					{name: "VER_A", index: 2},
				}},
			})
		}
		t.Run("UndefinedVersionIndex", func(t *testing.T) {
			// Symbol 1 requires version index 5, which no
			// verneed entry defines.
			raw := build([]uint16{0, 5, 2}, goodVerneed(), 1, true,
				strtab, ofs)
			expectImportError(t, raw, "undefined version index")
		})
		t.Run("ConflictingVersionIndex", func(t *testing.T) {
			verneed := synthVerneed(order, ofs, []verneedSpec{
				{file: "libx.so", versions: []versymSpec{
					{name: "VER_A", index: 2},
				}},
				{file: "liby.so", versions: []versymSpec{
					{name: "VER_B", index: 2},
				}},
			})
			raw := build([]uint16{0, 2, 2}, verneed, 2, true,
				strtab, ofs)
			expectImportError(t, raw, "conflicting version index")
		})
		t.Run("VerneedChainTooShort", func(t *testing.T) {
			// DT_VERNEEDNUM claims two entries, but the chain
			// ends after the first.
			raw := build([]uint16{0, 2, 2}, goodVerneed(), 2, true,
				strtab, ofs)
			expectImportError(t, raw, "verneed chain shorter than "+
				"DT_VERNEEDNUM")
		})
		t.Run("VerneedFinalNextNonzero", func(t *testing.T) {
			verneed := goodVerneed()
			// The single (and thus final) verneed entry's next
			// offset must be zero.
			binary.LittleEndian.PutUint32(verneed[12:], 16)
			raw := build([]uint16{0, 2, 2}, verneed, 1, true,
				strtab, ofs)
			expectImportError(t, raw, "final verneed with nonzero "+
				"next offset")
		})
		t.Run("VernauxChainTooShort", func(t *testing.T) {
			verneed := synthVerneed(order, ofs, []verneedSpec{
				{file: "libx.so", versions: []versymSpec{
					{name: "VER_A", index: 2},
					{name: "VER_B", index: 3},
				}},
			})
			// The first aux entry's next offset must be nonzero
			// since vn_cnt is 2.
			binary.LittleEndian.PutUint32(verneed[16+12:], 0)
			raw := build([]uint16{0, 2, 3}, verneed, 1, true,
				strtab, ofs)
			expectImportError(t, raw, "vernaux chain shorter than "+
				"vn_cnt")
		})
		t.Run("UnterminatedString", func(t *testing.T) {
			// "two" has no terminating null within DT_STRSZ.
			badStrtab := []byte("\x00one\x00two")
			badOfs := map[string]uint32{"one": 1, "two": 5}
			raw := build([]uint16{0, 1, 1}, []byte{}, 0, true,
				badStrtab, badOfs)
			expectImportError(t, raw, "unterminated symbol name")
		})
		t.Run("MissingVerneednum", func(t *testing.T) {
			raw := build([]uint16{0, 1, 1}, goodVerneed(), 0, false,
				strtab, ofs)
			expectImportError(t, raw, "DT_VERNEED without "+
				"DT_VERNEEDNUM")
		})
	})
}
