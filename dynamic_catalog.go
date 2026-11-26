package elf_reader

// This file implements ReadDynamicImports, a library entry point that
// recovers the catalog of imported (undefined) dynamic symbols from a 64-bit
// ELF image using only its program headers. It works even when the image
// contains no section table: the dynamic linking information is located
// through the PT_DYNAMIC segment, and the virtual addresses referenced by
// dynamic tags are resolved through the file-backed portions of PT_LOAD
// segments. The image is only inspected; it is never loaded or executed,
// and the ELF64File passed in is never modified.

import (
	"encoding/binary"
	"fmt"
)

// DynamicImport describes a single imported dynamic symbol: an undefined
// dynamic symbol table entry with a non-empty name, along with the symbol
// version and dependency file it is associated with when version
// requirements are present.
type DynamicImport struct {
	// Index is the symbol's index in the dynamic symbol table.
	Index uint32
	// Name is the symbol's name, from the dynamic string table.
	Name string
	// Binding is the symbol's binding (the high nibble of st_info).
	Binding uint8
	// Type is the symbol's type (the low nibble of st_info).
	Type uint8
	// RawVersym is the symbol's unmodified DT_VERSYM value, including the
	// "hidden" bit (0x8000). It is 1 when the file has no DT_VERSYM table.
	RawVersym uint16
	// VersionIndex is RawVersym with the hidden bit cleared. It is 1 when
	// the file has no DT_VERSYM table.
	VersionIndex uint16
	// VersionName is the name of the symbol version required from a
	// dependency, or "" if the symbol isn't associated with one.
	VersionName string
	// FileName is the dependency file providing the required version, or ""
	// if the symbol isn't associated with one.
	FileName string
}

// Dynamic tags (values of ELF64DynamicEntry.Tag) used by this file. They're
// kept unexported since the rest of the library refers to dynamic tags by
// their numeric values as well.
const (
	dynTagNull       = 0
	dynTagNeeded     = 1
	dynTagHash       = 4
	dynTagStrTab     = 5
	dynTagSymTab     = 6
	dynTagStrSz      = 10
	dynTagSymEnt     = 11
	dynTagGNUHash    = 0x6ffffef5
	dynTagVersym     = 0x6ffffff0
	dynTagVerNeed    = 0x6ffffffe
	dynTagVerNeedNum = 0x6fffffff
)

// The size of a single ELF64 symbol table entry, which DT_SYMENT must match.
const elf64DynamicSymbolSize = 24

// The bit in a DT_VERSYM value (or a Vernaux other field) marking a hidden
// symbol version.
const versymHiddenBit = 0x8000

// Returns true for dynamic tags that may legitimately appear multiple times
// with different values. All other tags are single-valued: repeats are only
// accepted when the values are identical.
func isMultiValueDynamicTag(tag int64) bool {
	switch tag {
	case dynTagNeeded:
		return true
	case 0x7ffffffa, 0x7ffffffb, 0x7ffffffc, 0x7ffffffd, 0x7ffffffe,
		0x7fffffff:
		// DT_CONFIG, DT_DEPAUDIT, DT_AUDIT, DT_AUXILIARY, DT_USED, and
		// DT_FILTER may all repeat, like DT_NEEDED.
		return true
	}
	return false
}

// dynamicTagSet collects the single-valued dynamic tags of one PT_DYNAMIC
// segment.
type dynamicTagSet map[int64]uint64

// Adds one dynamic entry to the set. Returns an error if a single-valued tag
// already exists with a different value.
func (s dynamicTagSet) Add(tag int64, value uint64) error {
	if isMultiValueDynamicTag(tag) {
		return nil
	}
	old, exists := s[tag]
	if exists {
		if old != value {
			return fmt.Errorf("Dynamic tag %d appears twice with different "+
				"values (0x%x and 0x%x)", tag, old, value)
		}
		return nil
	}
	s[tag] = value
	return nil
}

// loadMapping describes the file-backed portion of one PT_LOAD segment.
type loadMapping struct {
	vaddr    uint64
	fileOff  uint64
	fileSize uint64
	memSize  uint64
}

// addressReader reads file content using the virtual addresses of a 64-bit
// ELF image's PT_LOAD segments. Only file-backed bytes are readable;
// addresses in zero-filled segment tails or outside every mapping are
// errors. Adjacent mappings are concatenated, so a single read may cross
// from one segment's file-backed region into the next.
type addressReader struct {
	raw  []byte
	bo   binary.ByteOrder
	maps []loadMapping
}

// Builds an addressReader for the PT_LOAD segments of the given file.
func newAddressReader(f *ELF64File, bo binary.ByteOrder) *addressReader {
	maps := make([]loadMapping, 0, len(f.Segments))
	for i := range f.Segments {
		s := &(f.Segments[i])
		if s.Type != LoadableSegment {
			continue
		}
		maps = append(maps, loadMapping{
			vaddr:    s.VirtualAddress,
			fileOff:  s.FileOffset,
			fileSize: s.FileSize,
			memSize:  s.MemorySize,
		})
	}
	return &addressReader{raw: f.Raw, bo: bo, maps: maps}
}

// Reads size bytes of file-backed content starting at the given virtual
// address. The returned slice is always a fresh copy.
func (r *addressReader) Read(addr uint64, size uint64) ([]byte, error) {
	if size == 0 {
		return []byte{}, nil
	}
	if addr+size < addr {
		return nil, fmt.Errorf("Address range 0x%x + %d bytes overflows",
			addr, size)
	}
	if size > uint64(len(r.raw)) {
		return nil, fmt.Errorf("A read of %d bytes at address 0x%x exceeds "+
			"the file size", size, addr)
	}
	out := make([]byte, 0, size)
	current := addr
	remaining := size
	for remaining > 0 {
		var fileOff, available uint64
		found := false
		inZeroTail := false
		for i := range r.maps {
			m := &(r.maps[i])
			if current < m.vaddr {
				continue
			}
			offset := current - m.vaddr
			if offset < m.fileSize {
				candidate := m.fileOff + offset
				if candidate < m.fileOff {
					return nil, fmt.Errorf("File offset overflow while "+
						"mapping address 0x%x", current)
				}
				if !found {
					fileOff = candidate
					available = m.fileSize - offset
					found = true
					continue
				}
				if fileOff != candidate {
					return nil, fmt.Errorf("Address 0x%x is mapped by "+
						"multiple PT_LOAD segments with different file "+
						"offsets", current)
				}
				if m.fileSize-offset < available {
					available = m.fileSize - offset
				}
				continue
			}
			if offset < m.memSize {
				inZeroTail = true
			}
		}
		if !found {
			if inZeroTail {
				return nil, fmt.Errorf("Address 0x%x is in the zero-filled "+
					"tail of a PT_LOAD segment and has no file content",
					current)
			}
			return nil, fmt.Errorf("Address 0x%x is not backed by any "+
				"PT_LOAD segment", current)
		}
		n := available
		if n > remaining {
			n = remaining
		}
		if fileOff > uint64(len(r.raw)) || n > uint64(len(r.raw))-fileOff {
			return nil, fmt.Errorf("The file content for address 0x%x "+
				"extends past the end of the file", current)
		}
		out = append(out, r.raw[fileOff:fileOff+n]...)
		current += n
		remaining -= n
	}
	return out, nil
}

// Reads a single big- or little-endian uint32 at the given virtual address.
func (r *addressReader) Uint32(addr uint64) (uint32, error) {
	b, e := r.Read(addr, 4)
	if e != nil {
		return 0, e
	}
	return r.bo.Uint32(b), nil
}

// Reads the dynamic entries of the single PT_DYNAMIC segment, stopping at
// the DT_NULL terminator, and collects the single-valued tags.
func readDynamicTags(f *ELF64File, bo binary.ByteOrder) (dynamicTagSet,
	error) {
	dynamicSegment := -1
	for i := range f.Segments {
		if f.Segments[i].Type != DynamicLinkingSegment {
			continue
		}
		if dynamicSegment >= 0 {
			return nil, fmt.Errorf("Multiple PT_DYNAMIC segments found")
		}
		dynamicSegment = i
	}
	if dynamicSegment < 0 {
		return nil, fmt.Errorf("No PT_DYNAMIC segment found")
	}
	segment := &(f.Segments[dynamicSegment])
	start := segment.FileOffset
	if start > uint64(len(f.Raw)) {
		return nil, fmt.Errorf("The PT_DYNAMIC file offset 0x%x is past the "+
			"end of the file", start)
	}
	if segment.FileSize > uint64(len(f.Raw))-start {
		return nil, fmt.Errorf("The PT_DYNAMIC segment extends past the end " +
			"of the file")
	}
	content := f.Raw[start : start+segment.FileSize]
	tags := make(dynamicTagSet)
	foundNull := false
	for offset := 0; offset+16 <= len(content); offset += 16 {
		tag := int64(bo.Uint64(content[offset:]))
		value := bo.Uint64(content[offset+8:])
		if tag == dynTagNull {
			foundNull = true
			break
		}
		if e := tags.Add(tag, value); e != nil {
			return nil, e
		}
	}
	if !foundNull {
		return nil, fmt.Errorf("The PT_DYNAMIC segment isn't terminated by " +
			"a DT_NULL entry")
	}
	return tags, nil
}

// Determines the dynamic symbol count from the DT_HASH table at the given
// virtual address: the table's nchain field equals the number of symbols.
func hashTableSymbolCount(reader *addressReader, addr uint64) (uint64,
	error) {
	header, e := reader.Read(addr, 8)
	if e != nil {
		return 0, fmt.Errorf("Failed reading the DT_HASH header: %s", e)
	}
	return uint64(reader.bo.Uint32(header[4:8])), nil
}

// Determines the dynamic symbol count from the DT_GNU_HASH table at the
// given virtual address. Symbols below symoffset aren't hashed but are part
// of the symbol table, so the count includes them. The count of hashed
// symbols comes from walking the shared chain array, starting at the highest
// bucket's first symbol, until the entry with its low (terminator) bit set.
func gnuHashSymbolCount(reader *addressReader, addr uint64) (uint64, error) {
	header, e := reader.Read(addr, 16)
	if e != nil {
		return 0, fmt.Errorf("Failed reading the DT_GNU_HASH header: %s", e)
	}
	bo := reader.bo
	bucketCount := bo.Uint32(header[0:4])
	symbolOffset := bo.Uint32(header[4:8])
	bloomSize := bo.Uint32(header[8:12])
	if bucketCount == 0 {
		return 0, fmt.Errorf("The DT_GNU_HASH table has no buckets")
	}
	// Each bloom word is 8 bytes in a 64-bit file; buckets follow them.
	bucketsAddr := addr + 16 + uint64(bloomSize)*8
	buckets, e := reader.Read(bucketsAddr, uint64(bucketCount)*4)
	if e != nil {
		return 0, fmt.Errorf("Failed reading the DT_GNU_HASH buckets: %s", e)
	}
	var maxBucket uint32
	for i := uint32(0); i < bucketCount; i++ {
		b := bo.Uint32(buckets[i*4:])
		if b == 0 {
			// An empty bucket.
			continue
		}
		if b < symbolOffset {
			return 0, fmt.Errorf("DT_GNU_HASH bucket %d refers to symbol "+
				"%d, below the symbol offset %d", i, b, symbolOffset)
		}
		if b > maxBucket {
			maxBucket = b
		}
	}
	if maxBucket == 0 {
		// Every bucket is empty: only the symoffset prefix exists.
		return uint64(symbolOffset), nil
	}
	chainAddr := bucketsAddr + uint64(bucketCount)*4
	// Walk the chain of the bucket containing the last symbols. Every chain
	// entry must be file-backed, so the walk can't take more steps than the
	// file has 4-byte words.
	index := maxBucket
	limit := uint64(len(reader.raw))/4 + 1
	for steps := uint64(0); ; steps++ {
		if steps > limit {
			return 0, fmt.Errorf("The DT_GNU_HASH chain doesn't terminate")
		}
		entry, e := reader.Uint32(chainAddr +
			uint64(index-symbolOffset)*4)
		if e != nil {
			return 0, fmt.Errorf("Failed reading the DT_GNU_HASH chain: %s",
				e)
		}
		if entry&1 != 0 {
			return uint64(index) + 1, nil
		}
		index++
	}
}

// versionAssociation records which version name and dependency file a
// 15-bit version index maps to.
type versionAssociation struct {
	name string
	file string
}

// Reads a string from the dynamic string table content. The string must
// terminate within the table (whose size is DT_STRSZ).
func readDynamicString(strtab []byte, offset uint32) (string, error) {
	content, e := ReadStringAtOffset(offset, strtab)
	if e != nil {
		return "", e
	}
	return string(content), nil
}

// Parses the DT_VERNEED chain: count verneed records, each with its own
// chain of vernaux records. All offsets are relative to the record they're
// stored in (vn_aux to the containing verneed, vn_next and vna_next to the
// current record); they are never absolute file positions. Each chain must
// contain exactly the recorded number of entries, with a zero link on the
// final entry. Returns a map from 15-bit version index to its association.
func parseVersionNeeds(reader *addressReader, bo binary.ByteOrder,
	addr uint64, count uint64, strtab []byte) (map[uint16]versionAssociation,
	error) {
	associations := make(map[uint16]versionAssociation)
	current := addr
	for i := uint64(0); i < count; i++ {
		header, e := reader.Read(current, 16)
		if e != nil {
			return nil, fmt.Errorf("Failed reading version need %d: %s", i,
				e)
		}
		auxCount := bo.Uint16(header[2:4])
		fileOffset := bo.Uint32(header[4:8])
		auxOffset := bo.Uint32(header[8:12])
		next := bo.Uint32(header[12:16])
		last := (i + 1) == count
		if last && next != 0 {
			return nil, fmt.Errorf("The final version need (%d of %d) has "+
				"a non-zero link to the next record", i+1, count)
		}
		if !last && next == 0 {
			return nil, fmt.Errorf("The version need chain ends at record "+
				"%d, before the expected %d records", i+1, count)
		}
		fileName, e := readDynamicString(strtab, fileOffset)
		if e != nil {
			return nil, fmt.Errorf("Failed reading the file name of "+
				"version need %d: %s", i, e)
		}
		aux := current + uint64(auxOffset)
		for j := uint16(0); j < auxCount; j++ {
			auxData, e := reader.Read(aux, 16)
			if e != nil {
				return nil, fmt.Errorf("Failed reading aux record %d of "+
					"version need %d: %s", j, i, e)
			}
			other := bo.Uint16(auxData[6:8])
			nameOffset := bo.Uint32(auxData[8:12])
			auxNext := bo.Uint32(auxData[12:16])
			lastAux := (j + 1) == auxCount
			if lastAux && auxNext != 0 {
				return nil, fmt.Errorf("The final aux record (%d of %d) "+
					"of version need %d has a non-zero link to the next "+
					"record", j+1, auxCount, i)
			}
			if !lastAux && auxNext == 0 {
				return nil, fmt.Errorf("The aux chain of version need %d "+
					"ends at record %d, before the expected %d records", i,
					j+1, auxCount)
			}
			versionName, e := readDynamicString(strtab, nameOffset)
			if e != nil {
				return nil, fmt.Errorf("Failed reading the version name "+
					"of aux record %d of version need %d: %s", j, i, e)
			}
			index := other &^ versymHiddenBit
			association := versionAssociation{name: versionName,
				file: fileName}
			old, exists := associations[index]
			if exists && old != association {
				return nil, fmt.Errorf("Version index %d has conflicting "+
					"definitions (%q in %q vs %q in %q)", index, old.name,
					old.file, versionName, fileName)
			}
			associations[index] = association
			aux += uint64(auxNext)
		}
		current += uint64(next)
	}
	return associations, nil
}

// Builds the version index associations for an image, if it carries version
// information. Returns a nil map (and no error) when no symbol can require
// an association.
func versionAssociations(reader *addressReader, bo binary.ByteOrder,
	tags dynamicTagSet, strtab []byte, hasVersym bool) (
	map[uint16]versionAssociation, error) {
	if !hasVersym {
		return nil, nil
	}
	addr, hasAddr := tags[dynTagVerNeed]
	num, hasNum := tags[dynTagVerNeedNum]
	if hasAddr != hasNum {
		return nil, fmt.Errorf("Only one of DT_VERNEED and DT_VERNEEDNUM " +
			"is present in the dynamic segment")
	}
	if !hasAddr {
		return nil, nil
	}
	return parseVersionNeeds(reader, bo, addr, num, strtab)
}

// ReadDynamicImports catalogs the imported dynamic symbols of a 64-bit ELF
// image, using only its program headers; the image doesn't need a section
// table. The returned records cover the undefined (SHN_UNDEF) dynamic
// symbols with non-empty names, in their original symbol table order, and
// include the symbol version and dependency file required by each import
// when the image carries version requirement information. The image is only
// inspected, never loaded or executed, and the given ELF64File is left
// unmodified.
func ReadDynamicImports(f *ELF64File) ([]DynamicImport, error) {
	if f == nil {
		return nil, fmt.Errorf("Cannot read dynamic imports from a nil " +
			"ELF64File")
	}
	bo := f.Endianness
	if bo == nil {
		return nil, fmt.Errorf("The ELF64File's Endianness field isn't set")
	}
	tags, e := readDynamicTags(f, bo)
	if e != nil {
		return nil, e
	}
	reader := newAddressReader(f, bo)
	symtabAddr, ok := tags[dynTagSymTab]
	if !ok {
		return nil, fmt.Errorf("No DT_SYMTAB entry in the dynamic segment")
	}
	strtabAddr, ok := tags[dynTagStrTab]
	if !ok {
		return nil, fmt.Errorf("No DT_STRTAB entry in the dynamic segment")
	}
	strtabSize, ok := tags[dynTagStrSz]
	if !ok {
		return nil, fmt.Errorf("No DT_STRSZ entry in the dynamic segment")
	}
	symEnt, ok := tags[dynTagSymEnt]
	if !ok {
		return nil, fmt.Errorf("No DT_SYMENT entry in the dynamic segment")
	}
	if symEnt != elf64DynamicSymbolSize {
		return nil, fmt.Errorf("DT_SYMENT is %d, but ELF64 symbol entries "+
			"are %d bytes", symEnt, elf64DynamicSymbolSize)
	}
	// The symbol count must come from a hash table; it can't be guessed
	// from the string table or by scanning for a zero symbol.
	hashAddr, hasHash := tags[dynTagHash]
	gnuAddr, hasGNUHash := tags[dynTagGNUHash]
	if !hasHash && !hasGNUHash {
		return nil, fmt.Errorf("Unsupported image: no DT_HASH or " +
			"DT_GNU_HASH entry to determine the dynamic symbol count")
	}
	var symbolCount uint64
	if hasHash {
		symbolCount, e = hashTableSymbolCount(reader, hashAddr)
		if e != nil {
			return nil, e
		}
	}
	if hasGNUHash {
		gnuCount, e := gnuHashSymbolCount(reader, gnuAddr)
		if e != nil {
			return nil, e
		}
		if hasHash && gnuCount != symbolCount {
			return nil, fmt.Errorf("DT_HASH and DT_GNU_HASH disagree on "+
				"the dynamic symbol count (%d vs %d)", symbolCount,
				gnuCount)
		}
		symbolCount = gnuCount
	}
	strtab, e := reader.Read(strtabAddr, strtabSize)
	if e != nil {
		return nil, fmt.Errorf("Failed reading the dynamic string table: %s",
			e)
	}
	symbolBytes, e := reader.Read(symtabAddr,
		symbolCount*elf64DynamicSymbolSize)
	if e != nil {
		return nil, fmt.Errorf("Failed reading the dynamic symbol table: %s",
			e)
	}
	var versym []byte
	if versymAddr, ok := tags[dynTagVersym]; ok {
		versym, e = reader.Read(versymAddr, symbolCount*2)
		if e != nil {
			return nil, fmt.Errorf("Failed reading the version symbol "+
				"table: %s", e)
		}
	}
	versions, e := versionAssociations(reader, bo, tags, strtab,
		versym != nil)
	if e != nil {
		return nil, e
	}
	imports := make([]DynamicImport, 0)
	for i := uint64(0); i < symbolCount; i++ {
		symbol := symbolBytes[i*elf64DynamicSymbolSize:]
		// Section index 0 (SHN_UNDEF) marks an undefined symbol.
		if bo.Uint16(symbol[6:8]) != 0 {
			continue
		}
		nameOffset := bo.Uint32(symbol[0:4])
		if nameOffset == 0 {
			continue
		}
		name, e := ReadStringAtOffset(nameOffset, strtab)
		if e != nil {
			return nil, fmt.Errorf("Failed reading the name of dynamic "+
				"symbol %d: %s", i, e)
		}
		if len(name) == 0 {
			continue
		}
		record := DynamicImport{
			Index:        uint32(i),
			Name:         string(name),
			Binding:      symbol[4] >> 4,
			Type:         symbol[4] & 0xf,
			RawVersym:    1,
			VersionIndex: 1,
		}
		if versym != nil {
			raw := bo.Uint16(versym[i*2:])
			record.RawVersym = raw
			record.VersionIndex = raw &^ versymHiddenBit
			// Version indexes 0 (local) and 1 (global) never refer to a
			// dependency.
			if record.VersionIndex >= 2 {
				association, ok := versions[record.VersionIndex]
				if !ok {
					return nil, fmt.Errorf("Dynamic symbol %d (%s) refers "+
						"to version index %d, which has no DT_VERNEED "+
						"definition", i, name, record.VersionIndex)
				}
				record.VersionName = association.name
				record.FileName = association.file
			}
		}
		imports = append(imports, record)
	}
	return imports, nil
}
