package elf_reader

// This file implements ReadDynamicImports, which recovers the list of
// imported (undefined) dynamic symbols from a 64-bit ELF image using only
// the program headers. It's intended for images that may not contain a
// section table at all: everything is located through the PT_DYNAMIC segment
// and the file-backed portions of PT_LOAD segments. The image is only
// inspected; it is never executed or loaded.

import (
	"bytes"
	"encoding/binary"
	"fmt"
)

// Dynamic tag values used when building the import catalog. The rest of the
// package refers to dynamic tags numerically, so these are kept private.
const (
	dynamicTagHash       = int64(4)
	dynamicTagStrtab     = int64(5)
	dynamicTagSymtab     = int64(6)
	dynamicTagStrsz      = int64(10)
	dynamicTagSyment     = int64(11)
	dynamicTagGnuHash    = int64(0x6ffffef5)
	dynamicTagVersym     = int64(0x6ffffff0)
	dynamicTagVerneed    = int64(0x6ffffffe)
	dynamicTagVerneednum = int64(0x6fffffff)
)

// A single imported dynamic symbol, as produced by ReadDynamicImports.
type DynamicImport struct {
	// The symbol's index in the dynamic symbol table.
	SymbolIndex uint32
	// The symbol's name.
	Name string
	// The symbol's binding (e.g. 1 for global, 2 for weak).
	Binding uint8
	// The symbol's type (e.g. 1 for object, 2 for function).
	SymbolType uint8
	// The raw value from the file's version symbol table, including the
	// "hidden" bit (0x8000). Set to 1 if the file has no DT_VERSYM entry.
	RawVersion uint16
	// The version index: the raw version with the hidden bit removed. Set
	// to 1 if the file has no DT_VERSYM entry.
	VersionIndex uint16
	// The name of the version the symbol requires, from the DT_VERNEED
	// chain. Empty for version indices 0 and 1, and for files without
	// DT_VERSYM.
	VersionName string
	// The name of the dependency file that provides the required version.
	// Empty under the same conditions as VersionName.
	DependencyFile string
}

// Holds the association between a version index and the version requirement
// that defines it.
type versionRequirement struct {
	versionName string
	fileName    string
}

// Reads the given range of virtual addresses from the file-backed portions
// of the file's PT_LOAD segments. Adjacent mappings are stitched together.
// Returns an error if any part of the range falls in a hole between
// mappings, in a segment's zero-filled tail (past its file size), or is
// covered by multiple mappings backed by different file contents.
func readVirtualAddress(f *ELF64File, address uint64, size uint64) ([]byte,
	error) {
	if size == 0 {
		return []byte{}, nil
	}
	end := address + size
	if end < address {
		return nil, fmt.Errorf("Virtual address range 0x%x + 0x%x overflows",
			address, size)
	}
	toReturn := make([]byte, 0, size)
	position := address
	for position < end {
		chunkEnd, data, e := virtualChunk(f, position, end)
		if e != nil {
			return nil, e
		}
		toReturn = append(toReturn, data...)
		position = chunkEnd
	}
	return toReturn, nil
}

// Returns the largest contiguous chunk starting at position that comes from
// a single PT_LOAD segment's file-backed bytes, limited to the given end
// address. The returned slice aliases the file's raw contents.
func virtualChunk(f *ELF64File, position uint64, limit uint64) (uint64,
	[]byte, error) {
	chunkEnd := limit
	source := -1
	inZeroTail := false
	for i := range f.Segments {
		segment := &(f.Segments[i])
		if segment.Type != LoadableSegment {
			continue
		}
		fileEnd := segment.VirtualAddress + segment.FileSize
		memoryEnd := segment.VirtualAddress + segment.MemorySize
		if (fileEnd < segment.VirtualAddress) ||
			(memoryEnd < segment.VirtualAddress) {
			return 0, nil, fmt.Errorf("Segment %d has an overflowing "+
				"address range", i)
		}
		// A segment starting later truncates the current chunk, so that the
		// set of covering segments stays constant within a chunk.
		if (segment.VirtualAddress > position) &&
			(segment.VirtualAddress < chunkEnd) {
			chunkEnd = segment.VirtualAddress
		}
		if (position >= segment.VirtualAddress) && (position < fileEnd) {
			if (segment.FileOffset > uint64(len(f.Raw))) ||
				(segment.FileSize > uint64(len(f.Raw))-segment.FileOffset) {
				return 0, nil, fmt.Errorf("Segment %d's file range "+
					"exceeds the file size", i)
			}
			if source < 0 {
				source = i
			} else {
				// A second mapping covers this address: it's only
				// acceptable if both mappings are backed by identical
				// file contents.
				other := &(f.Segments[source])
				sharedEnd := chunkEnd
				if fileEnd < sharedEnd {
					sharedEnd = fileEnd
				}
				otherFileEnd := other.VirtualAddress + other.FileSize
				if otherFileEnd < sharedEnd {
					sharedEnd = otherFileEnd
				}
				aStart := other.FileOffset + (position -
					other.VirtualAddress)
				aEnd := other.FileOffset + (sharedEnd -
					other.VirtualAddress)
				bStart := segment.FileOffset + (position -
					segment.VirtualAddress)
				bEnd := segment.FileOffset + (sharedEnd -
					segment.VirtualAddress)
				if !bytes.Equal(f.Raw[aStart:aEnd], f.Raw[bStart:bEnd]) {
					return 0, nil, fmt.Errorf("Segments %d and %d map "+
						"virtual address 0x%x to different file "+
						"contents", source, i, position)
				}
			}
			if fileEnd < chunkEnd {
				chunkEnd = fileEnd
			}
		} else if (position >= segment.VirtualAddress) &&
			(position < memoryEnd) {
			inZeroTail = true
		}
	}
	if source < 0 {
		if inZeroTail {
			return 0, nil, fmt.Errorf("Virtual address 0x%x is in a "+
				"segment's zero-filled tail", position)
		}
		return 0, nil, fmt.Errorf("No loadable segment maps virtual "+
			"address 0x%x to file contents", position)
	}
	segment := &(f.Segments[source])
	start := segment.FileOffset + (position - segment.VirtualAddress)
	end := segment.FileOffset + (chunkEnd - segment.VirtualAddress)
	return chunkEnd, f.Raw[start:end], nil
}

// Reads and parses the entries of the file's single PT_DYNAMIC segment, up
// to (but not including) the terminating DT_NULL entry.
func dynamicSegmentEntries(f *ELF64File) ([]ELF64DynamicEntry, error) {
	dynamicIndex := -1
	for i := range f.Segments {
		if f.Segments[i].Type != DynamicLinkingSegment {
			continue
		}
		if dynamicIndex >= 0 {
			return nil, fmt.Errorf("Expected exactly one PT_DYNAMIC " +
				"segment, but found more")
		}
		dynamicIndex = i
	}
	if dynamicIndex < 0 {
		return nil, fmt.Errorf("The file doesn't contain a PT_DYNAMIC " +
			"segment")
	}
	content, e := f.GetSegmentContent(uint16(dynamicIndex))
	if e != nil {
		return nil, fmt.Errorf("Failed reading the PT_DYNAMIC segment: %s", e)
	}
	entrySize := binary.Size(ELF64DynamicEntry{})
	if (len(content) % entrySize) != 0 {
		return nil, fmt.Errorf("The PT_DYNAMIC segment's size (%d) isn't "+
			"a multiple of the entry size (%d)", len(content), entrySize)
	}
	parsed := make([]ELF64DynamicEntry, len(content)/entrySize)
	e = binary.Read(bytes.NewReader(content), f.Endianness, parsed)
	if e != nil {
		return nil, fmt.Errorf("Failed parsing the PT_DYNAMIC segment: %s",
			e)
	}
	for i := range parsed {
		if parsed[i].Tag == 0 {
			return parsed[:i], nil
		}
	}
	return nil, fmt.Errorf("The PT_DYNAMIC segment doesn't contain a " +
		"DT_NULL entry")
}

// Collects the values of the single-valued dynamic tags that
// ReadDynamicImports cares about. Multi-valued tags (such as DT_NEEDED) and
// unrelated tags are ignored. A repeated single-valued tag is only accepted
// if every occurrence carries the same value.
func collectDynamicTags(entries []ELF64DynamicEntry) (map[int64]uint64,
	error) {
	toReturn := make(map[int64]uint64)
	for i := range entries {
		tag := int64(entries[i].Tag)
		switch tag {
		case dynamicTagHash, dynamicTagStrtab, dynamicTagSymtab,
			dynamicTagStrsz, dynamicTagSyment, dynamicTagGnuHash,
			dynamicTagVersym, dynamicTagVerneed, dynamicTagVerneednum:
			value := entries[i].Value
			if existing, ok := toReturn[tag]; ok {
				if existing != value {
					return nil, fmt.Errorf("The dynamic table "+
						"contains conflicting values 0x%x and "+
						"0x%x for tag 0x%x", existing, value,
						tag)
				}
				continue
			}
			toReturn[tag] = value
		default:
			// Not a tag we need; skip it.
		}
	}
	return toReturn, nil
}

// Determines the number of dynamic symbols from a DT_HASH table: the nchain
// value in its header.
func elfHashSymbolCount(f *ELF64File, address uint64) (uint32, error) {
	header, e := readVirtualAddress(f, address, 8)
	if e != nil {
		return 0, fmt.Errorf("Failed reading the DT_HASH header: %s", e)
	}
	return f.Endianness.Uint32(header[4:8]), nil
}

// Determines the number of dynamic symbols from a DT_GNU_HASH table: one
// past the highest symbol index reachable from any bucket's chain. The
// symoffset prefix of the symbol table is included in the count.
func gnuHashSymbolCount(f *ELF64File, address uint64) (uint32, error) {
	header, e := readVirtualAddress(f, address, 16)
	if e != nil {
		return 0, fmt.Errorf("Failed reading the GNU hash table header: %s",
			e)
	}
	bucketCount := f.Endianness.Uint32(header[0:4])
	symbolOffset := f.Endianness.Uint32(header[4:8])
	bloomSize := f.Endianness.Uint32(header[8:12])
	// Each bloom filter word is 8 bytes in a 64-bit file.
	bucketsAddress := address + 16 + uint64(bloomSize)*8
	if bucketsAddress < address {
		return 0, fmt.Errorf("The GNU hash table's bloom filter size " +
			"overflows")
	}
	bucketBytes, e := readVirtualAddress(f, bucketsAddress,
		uint64(bucketCount)*4)
	if e != nil {
		return 0, fmt.Errorf("Failed reading the GNU hash buckets: %s", e)
	}
	chainAddress := bucketsAddress + uint64(bucketCount)*4
	count := symbolOffset
	for i := uint32(0); i < bucketCount; i++ {
		bucket := f.Endianness.Uint32(bucketBytes[i*4 : i*4+4])
		if bucket == 0 {
			// An empty bucket.
			continue
		}
		if bucket < symbolOffset {
			return 0, fmt.Errorf("GNU hash bucket %d refers to "+
				"symbol %d, below the symbol offset %d", i, bucket,
				symbolOffset)
		}
		// Walk the chain until an entry with the low bit set; chains
		// of different buckets may share their tails.
		index := bucket
		for {
			chainBytes, e := readVirtualAddress(f, chainAddress+
				uint64(index-symbolOffset)*4, 4)
			if e != nil {
				return 0, fmt.Errorf("Failed reading the GNU hash "+
					"chain at symbol %d: %s", index, e)
			}
			index++
			if (f.Endianness.Uint32(chainBytes) & 1) != 0 {
				break
			}
		}
		if index > count {
			count = index
		}
	}
	return count, nil
}

// Reads a string from the dynamic string table, requiring it to terminate
// within the table (whose size comes from DT_STRSZ).
func readDynamicString(stringTable []byte, offset uint32) (string, error) {
	raw, e := ReadStringAtOffset(offset, stringTable)
	if e != nil {
		return "", e
	}
	return string(raw), nil
}

// Parses the DT_VERNEED chain at the given virtual address, expecting
// exactly count verneed entries. Returns a map from version index (the low
// 15 bits of each vernaux's "other" field) to the version and file names.
// All offsets in the chain are relative to the structure containing them,
// and the chain links must be consistent with the entry counts: every
// non-final entry needs a nonzero next offset, and every final entry needs
// a zero one.
func parseVersionNeeds(f *ELF64File, address uint64, count uint32,
	stringTable []byte) (map[uint16]versionRequirement, error) {
	requirements := make(map[uint16]versionRequirement)
	needAddress := address
	for i := uint32(0); i < count; i++ {
		raw, e := readVirtualAddress(f, needAddress, 16)
		if e != nil {
			return nil, fmt.Errorf("Failed reading version need %d: %s",
				i, e)
		}
		auxCount := f.Endianness.Uint16(raw[2:4])
		fileOffset := f.Endianness.Uint32(raw[4:8])
		auxOffset := f.Endianness.Uint32(raw[8:12])
		nextOffset := f.Endianness.Uint32(raw[12:16])
		fileName, e := readDynamicString(stringTable, fileOffset)
		if e != nil {
			return nil, fmt.Errorf("Failed reading the file name for "+
				"version need %d: %s", i, e)
		}
		if auxCount == 0 {
			return nil, fmt.Errorf("Version need %d (%s) contains no "+
				"auxiliary entries", i, fileName)
		}
		auxAddress := needAddress + uint64(auxOffset)
		for j := uint16(0); j < auxCount; j++ {
			auxRaw, e := readVirtualAddress(f, auxAddress, 16)
			if e != nil {
				return nil, fmt.Errorf("Failed reading version aux "+
					"%d of need %d: %s", j, i, e)
			}
			other := f.Endianness.Uint16(auxRaw[6:8])
			nameOffset := f.Endianness.Uint32(auxRaw[8:12])
			auxNext := f.Endianness.Uint32(auxRaw[12:16])
			versionName, e := readDynamicString(stringTable, nameOffset)
			if e != nil {
				return nil, fmt.Errorf("Failed reading the version "+
					"name for aux %d of need %d: %s", j, i, e)
			}
			index := other & 0x7fff
			if existing, ok := requirements[index]; ok {
				if (existing.versionName != versionName) ||
					(existing.fileName != fileName) {
					return nil, fmt.Errorf("Version index %d "+
						"has conflicting definitions (%s "+
						"in %s vs %s in %s)", index,
						existing.versionName,
						existing.fileName, versionName,
						fileName)
				}
			} else {
				requirements[index] = versionRequirement{
					versionName: versionName,
					fileName:    fileName,
				}
			}
			if j == auxCount-1 {
				if auxNext != 0 {
					return nil, fmt.Errorf("The last version "+
						"aux (%d) of need %d has a "+
						"nonzero next offset %d", j, i,
						auxNext)
				}
			} else if auxNext == 0 {
				return nil, fmt.Errorf("Version aux %d of need %d "+
					"ends the chain before the expected %d "+
					"entries", j, i, auxCount)
			}
			auxAddress += uint64(auxNext)
		}
		if i == count-1 {
			if nextOffset != 0 {
				return nil, fmt.Errorf("The last version need (%d "+
					"of %d) has a nonzero next offset %d", i,
					count, nextOffset)
			}
		} else if nextOffset == 0 {
			return nil, fmt.Errorf("The version need chain ends at "+
				"entry %d, but DT_VERNEEDNUM is %d", i, count)
		}
		needAddress += uint64(nextOffset)
	}
	return requirements, nil
}

// Returns the list of imported (undefined) dynamic symbols in the given
// 64-bit ELF file, sorted by their original dynamic symbol table indices.
// Only the program headers are used, so this works on images without a
// section table. Each returned record contains the symbol's index, name,
// binding, type, raw version value, version index (with the hidden bit
// removed), required version name, and dependency file name. Only undefined
// symbols with non-empty names are included; duplicate names are preserved.
// The file is only read, never modified, and the returned records don't
// borrow the file's mutable bytes.
func ReadDynamicImports(f *ELF64File) ([]DynamicImport, error) {
	if f == nil {
		return nil, fmt.Errorf("Cannot read dynamic imports from a nil " +
			"file")
	}
	entries, e := dynamicSegmentEntries(f)
	if e != nil {
		return nil, e
	}
	tags, e := collectDynamicTags(entries)
	if e != nil {
		return nil, e
	}
	symtabAddress, ok := tags[dynamicTagSymtab]
	if !ok {
		return nil, fmt.Errorf("The dynamic table is missing DT_SYMTAB")
	}
	strtabAddress, ok := tags[dynamicTagStrtab]
	if !ok {
		return nil, fmt.Errorf("The dynamic table is missing DT_STRTAB")
	}
	stringTableSize, ok := tags[dynamicTagStrsz]
	if !ok {
		return nil, fmt.Errorf("The dynamic table is missing DT_STRSZ")
	}
	symbolEntrySize, ok := tags[dynamicTagSyment]
	if !ok {
		return nil, fmt.Errorf("The dynamic table is missing DT_SYMENT")
	}
	if symbolEntrySize != uint64(binary.Size(ELF64Symbol{})) {
		return nil, fmt.Errorf("DT_SYMENT is %d, but 64-bit symbols are "+
			"%d bytes", symbolEntrySize, binary.Size(ELF64Symbol{}))
	}
	hashAddress, hasHash := tags[dynamicTagHash]
	gnuHashAddress, hasGnuHash := tags[dynamicTagGnuHash]
	if !hasHash && !hasGnuHash {
		return nil, fmt.Errorf("Counting dynamic symbols requires DT_HASH " +
			"or DT_GNU_HASH, but the dynamic table has neither")
	}
	var symbolCount uint32
	if hasHash {
		symbolCount, e = elfHashSymbolCount(f, hashAddress)
		if e != nil {
			return nil, e
		}
	}
	if hasGnuHash {
		gnuCount, gnuError := gnuHashSymbolCount(f, gnuHashAddress)
		if gnuError != nil {
			return nil, gnuError
		}
		if hasHash && (gnuCount != symbolCount) {
			return nil, fmt.Errorf("DT_HASH implies %d symbols but "+
				"DT_GNU_HASH implies %d", symbolCount, gnuCount)
		}
		symbolCount = gnuCount
	}
	symbolSize := uint64(binary.Size(ELF64Symbol{}))
	symbolBytes, e := readVirtualAddress(f, symtabAddress,
		uint64(symbolCount)*symbolSize)
	if e != nil {
		return nil, fmt.Errorf("Failed reading the dynamic symbol table: "+
			"%s", e)
	}
	symbols := make([]ELF64Symbol, symbolCount)
	e = binary.Read(bytes.NewReader(symbolBytes), f.Endianness, symbols)
	if e != nil {
		return nil, fmt.Errorf("Failed parsing the dynamic symbol table: "+
			"%s", e)
	}
	stringTable, e := readVirtualAddress(f, strtabAddress, stringTableSize)
	if e != nil {
		return nil, fmt.Errorf("Failed reading the dynamic string table: "+
			"%s", e)
	}
	versymAddress, hasVersym := tags[dynamicTagVersym]
	var versions []uint16
	var requirements map[uint16]versionRequirement
	if hasVersym {
		versionBytes, versionError := readVirtualAddress(f,
			versymAddress, uint64(symbolCount)*2)
		if versionError != nil {
			return nil, fmt.Errorf("Failed reading the version symbol "+
				"table: %s", versionError)
		}
		versions = make([]uint16, symbolCount)
		versionError = binary.Read(bytes.NewReader(versionBytes),
			f.Endianness, versions)
		if versionError != nil {
			return nil, fmt.Errorf("Failed parsing the version symbol "+
				"table: %s", versionError)
		}
		verneedAddress, hasVerneed := tags[dynamicTagVerneed]
		verneedCount, hasVerneednum := tags[dynamicTagVerneednum]
		if hasVerneed != hasVerneednum {
			return nil, fmt.Errorf("DT_VERNEED and DT_VERNEEDNUM must " +
				"either both be present or both be absent")
		}
		requirements = make(map[uint16]versionRequirement)
		if hasVerneed {
			if verneedCount > 0xffffffff {
				return nil, fmt.Errorf("DT_VERNEEDNUM is "+
					"unreasonably large: %d", verneedCount)
			}
			requirements, versionError = parseVersionNeeds(f,
				verneedAddress, uint32(verneedCount), stringTable)
			if versionError != nil {
				return nil, versionError
			}
		}
	}
	var toReturn []DynamicImport
	for i := uint32(0); i < symbolCount; i++ {
		symbol := &(symbols[i])
		// Only undefined symbols with non-empty names are imports.
		if symbol.SectionIndex != 0 {
			continue
		}
		if symbol.Name == 0 {
			continue
		}
		name, nameError := ReadStringAtOffset(symbol.Name, stringTable)
		if nameError != nil {
			return nil, fmt.Errorf("Failed reading the name of "+
				"dynamic symbol %d: %s", i, nameError)
		}
		if len(name) == 0 {
			continue
		}
		record := DynamicImport{
			SymbolIndex:  i,
			Name:         string(name),
			Binding:      symbol.Info.Binding(),
			SymbolType:   symbol.Info.SymbolType(),
			RawVersion:   1,
			VersionIndex: 1,
		}
		if hasVersym {
			record.RawVersion = versions[i]
			record.VersionIndex = versions[i] & 0x7fff
			// Version indices 0 and 1 aren't associated with any
			// dependency.
			if record.VersionIndex >= 2 {
				requirement, ok := requirements[record.VersionIndex]
				if !ok {
					return nil, fmt.Errorf("Symbol %d (%s) "+
						"requires version index %d, which "+
						"no DT_VERNEED entry defines", i,
						record.Name, record.VersionIndex)
				}
				record.VersionName = requirement.versionName
				record.DependencyFile = requirement.fileName
			}
		}
		toReturn = append(toReturn, record)
	}
	return toReturn, nil
}
