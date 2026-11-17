// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Package dwarf provides access to DWARF debugging information loaded from
// executable files, as defined in the DWARF 2.0 Standard at
// http://dwarfstd.org/doc/dwarf-2.0.0.pdf
package dwarf

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// Data represents the DWARF debugging information
// loaded from an executable file (for example, an ELF or Mach-O executable).
type Data struct {
	// raw data
	abbrev   []byte
	aranges  []byte
	frame    []byte
	info     []byte
	line     []byte
	pubnames []byte
	ranges   []byte
	str      []byte

	// New sections added in DWARF 5.
	addr       []byte
	lineStr    []byte
	strOffsets []byte
	rngLists   []byte
	locLists   []byte

	// parsed data
	abbrevCache map[uint64]abbrevTable
	bigEndian   bool
	order       binary.ByteOrder
	typeCache   map[Offset]Type
	typeSigs    map[uint64]*typeUnit
	hashes      map[string]*Hash
	names       *DebugNames
	unit        []unit
	cunits      []Offset

	cu *Entry // current compilation unit
}

var errSegmentSelector = errors.New("non-zero segment_selector size not supported")
var ErrHashNotFound = errors.New("hash not found")

// New returns a new Data object initialized from the given parameters.
// Rather than calling this function directly, clients should typically use
// the DWARF method of the File type of the appropriate package debug/elf,
// debug/macho, or debug/pe.
//
// The []byte arguments are the data from the corresponding debug section
// in the object file; for example, for an ELF object, abbrev is the contents of
// the ".debug_abbrev" section.
func New(abbrev, aranges, frame, info, line, pubnames, ranges, str []byte) (*Data, error) {
	d := &Data{
		abbrev:      abbrev,
		aranges:     aranges,
		frame:       frame,
		info:        info,
		line:        line,
		pubnames:    pubnames,
		ranges:      ranges,
		str:         str,
		abbrevCache: make(map[uint64]abbrevTable),
		typeCache:   make(map[Offset]Type),
		typeSigs:    make(map[uint64]*typeUnit),
		hashes:      make(map[string]*Hash),
	}

	// Sniff .debug_info to figure out byte order.
	// 32-bit DWARF: 4 byte length, 2 byte version.
	// 64-bit DWARf: 4 bytes of 0xff, 8 byte length, 2 byte version.
	if len(d.info) < 6 {
		return nil, DecodeError{"info", Offset(len(d.info)), "too short", nil}
	}
	offset := 4
	if d.info[0] == 0xff && d.info[1] == 0xff && d.info[2] == 0xff && d.info[3] == 0xff {
		if len(d.info) < 14 {
			return nil, DecodeError{"info", Offset(len(d.info)), "too short", nil}
		}
		offset = 12
	}
	// Fetch the version, a tiny 16-bit number (1, 2, 3, 4, 5).
	x, y := d.info[offset], d.info[offset+1]
	switch {
	case x == 0 && y == 0:
		return nil, DecodeError{"info", 4, "unsupported version 0", nil}
	case x == 0:
		d.bigEndian = true
		d.order = binary.BigEndian
	case y == 0:
		d.bigEndian = false
		d.order = binary.LittleEndian
	default:
		return nil, DecodeError{"info", 4, "cannot determine byte order", nil}
	}

	u, err := d.parseUnits()
	if err != nil {
		return nil, err
	}
	d.unit = u
	return d, nil
}

// NewReaderAt returns a Data object whose .debug_info and .debug_str
// sections are read lazily from [io.ReaderAt] sources instead of being
// held entirely in memory. This makes it possible to inspect DWARF
// whose section offsets exceed 4 GiB without allocating the sections up
// front: construction reads only each unit header (enough to determine
// byte order and build the unit directory), and DIE bodies plus the
// whole string section are fetched on demand as [Reader], [Reader.Seek],
// [Reader.SkipChildren] and [Data.Type] need them.
//
// The abbrev slice is the full .debug_abbrev section. The info Section
// represents .debug_info and the str Section represents .debug_str;
// every other section, when needed, must still be supplied through
// [Data.AddSection] and the other slice-based entry points.
//
// Offsets reported and accepted by the returned Data are absolute
// offsets into the logical sections and may use the full 64-bit range;
// the high bits are preserved rather than compacted away. The caller
// retains ownership of both Section readers: the package does not close
// them.
func NewReaderAt(abbrev []byte, info, str Section) (*Data, error) {
	infoSrc, err := newSource("info", info)
	if err != nil {
		return nil, err
	}
	strSrc, err := newSource("str", str)
	if err != nil {
		return nil, err
	}
	d := &Data{
		abbrev:      abbrev,
		abbrevCache: make(map[uint64]abbrevTable),
		typeCache:   make(map[Offset]Type),
		typeSigs:    make(map[uint64]*typeUnit),
		hashes:      make(map[string]*Hash),
	}
	registerBackends(d, infoSrc, strSrc)

	// Sniff the byte order from the first unit header without
	// materializing the section.
	head := make([]byte, 4)
	if _, err := infoSrc.readAt(head, 0); err != nil {
		return nil, err
	}
	versOff := Offset(4)
	if head[0] == 0xff && head[1] == 0xff && head[2] == 0xff && head[3] == 0xff {
		versOff = 12
	}
	vers := make([]byte, 2)
	if _, err := infoSrc.readAt(vers, versOff); err != nil {
		return nil, err
	}
	x, y := vers[0], vers[1]
	switch {
	case x == 0 && y == 0:
		return nil, DecodeError{"info", versOff, "unsupported version 0", nil}
	case x == 0:
		d.bigEndian = true
		d.order = binary.BigEndian
	case y == 0:
		d.bigEndian = false
		d.order = binary.LittleEndian
	default:
		return nil, DecodeError{"info", versOff, "cannot determine byte order", nil}
	}

	u, err := d.parseUnits()
	if err != nil {
		return nil, err
	}
	d.unit = u
	return d, nil
}

// AddTypes will add one .debug_types section to the DWARF data. A
// typical object with DWARF version 4 debug info will have multiple
// .debug_types sections. The name is used for error reporting only,
// and serves to distinguish one .debug_types section from another.
func (d *Data) AddTypes(name string, types []byte) error {
	return d.parseTypes(name, types)
}

// AddSection adds another DWARF section by name. The name should be a
// DWARF section name such as ".debug_addr", ".debug_str_offsets", and
// so forth. This approach is used for new DWARF sections added in
// DWARF 5 and later.
func (d *Data) AddSection(name string, contents []byte) error {
	var err error
	switch name {
	case ".debug_addr":
		d.addr = contents
	case ".debug_line_str":
		d.lineStr = contents
	case ".debug_str_offsets":
		d.strOffsets = contents
	case ".debug_rnglists":
		d.rngLists = contents
	case ".debug_loclists":
		d.locLists = contents
	}
	// Just ignore names that we don't yet support.
	return err
}

// AddNames will add one .debug_names section to the DWARF data.
func (d *Data) AddNames(name string, contents []byte) error {
	return d.parseNames(name, contents)
}

func (d *Data) AddHashes(name string, contents []byte) error {
	return d.parseHashes(name, contents)
}

func (d *Data) LookupType(name string) (Offset, error) {
	thash, ok := d.hashes["types"]
	if !ok {
		return 0, fmt.Errorf("failed to find '__DWARF.__apple_types' hash data: %w", ErrHashNotFound)
	}
	c, err := thash.lookup(name)
	if err != nil {
		return 0, err
	}
	return c.GetFirstOffset(), nil
}

func (d *Data) DumpTypes() (Entries, error) {
	thash, ok := d.hashes["types"]
	if !ok {
		return nil, fmt.Errorf("failed to find '__DWARF.__apple_types' hash data: %w", ErrHashNotFound)
	}
	return thash.dump()
}

func (d *Data) LookupName(name string) (Offset, error) {
	thash, ok := d.hashes["names"]
	if !ok {
		return 0, fmt.Errorf("failed to find '__DWARF.__apple_names' hash data: %w", ErrHashNotFound)
	}
	c, err := thash.lookup(name)
	if err != nil {
		return 0, err
	}
	return c.GetFirstOffset(), nil
}

func (d *Data) DumpNames() (Entries, error) {
	thash, ok := d.hashes["names"]
	if !ok {
		return nil, fmt.Errorf("failed to find '__DWARF.__apple_names' hash data: %w", ErrHashNotFound)
	}
	return thash.dump()
}

func (d *Data) LookupNamespace(name string) (Offset, error) {
	thash, ok := d.hashes["namespac"]
	if !ok {
		return 0, fmt.Errorf("failed to find '__DWARF.__apple_namespac' hash data: %w", ErrHashNotFound)
	}
	c, err := thash.lookup(name)
	if err != nil {
		return 0, err
	}
	return c.GetFirstOffset(), nil
}

func (d *Data) DumpNamespaces() (Entries, error) {
	thash, ok := d.hashes["namespac"]
	if !ok {
		return nil, fmt.Errorf("failed to find '__DWARF.__apple_namespac' hash data: %w", ErrHashNotFound)
	}
	return thash.dump()
}

func (d *Data) LookupObjC(name string) (Offset, error) {
	thash, ok := d.hashes["objc"]
	if !ok {
		return 0, fmt.Errorf("failed to find '__DWARF.__apple_objc' hash data: %w", ErrHashNotFound)
	}
	c, err := thash.lookup(name)
	if err != nil {
		return 0, err
	}
	return c.GetFirstOffset(), nil
}

func (d *Data) DumpObjC() (Entries, error) {
	thash, ok := d.hashes["objc"]
	if !ok {
		return nil, fmt.Errorf("failed to find '__DWARF.__apple_objc' hash data: %w", ErrHashNotFound)
	}
	return thash.dump()
}
