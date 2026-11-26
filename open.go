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

	// ReaderAt-backed counterparts of info and str. When set, the
	// corresponding slice above is left nil and bytes are fetched
	// on demand. The Readers are never closed by this package.
	infoSec *remoteSection
	strSec  *remoteSection

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
	if err := d.init(); err != nil {
		return nil, err
	}
	return d, nil
}

// NewReaderAt returns a Data object whose .debug_info and .debug_str
// sections are read lazily from ReaderAt-backed Sections instead of
// being supplied as complete []byte values. This makes it possible to
// inspect DWARF whose sections, and the compilation units, types and
// strings within them, extend past 4 GiB.
//
// Only the unit headers needed to determine byte order and build the
// unit directory are read during construction; DIE bodies and the
// string section contents are fetched on demand. The remaining
// sections (.debug_abbrev and any section added through AddSection or
// AddTypes) still use the ordinary []byte interfaces.
//
// The Readers inside info and str are not closed by this package and
// their lifetime is managed by the caller. Offsets are always reported
// and accepted using the original section offsets; the source is never
// renumbered or compacted.
func NewReaderAt(abbrev []byte, info, str Section) (*Data, error) {
	if info.Reader == nil {
		return nil, errors.New("dwarf: NewReaderAt requires a non-nil info Section reader")
	}
	if str.Reader == nil {
		return nil, errors.New("dwarf: NewReaderAt requires a non-nil str Section reader")
	}
	if info.Size < 0 || str.Size < 0 {
		return nil, errors.New("dwarf: NewReaderAt requires non-negative section sizes")
	}
	d := &Data{
		abbrev:      abbrev,
		abbrevCache: make(map[uint64]abbrevTable),
		typeCache:   make(map[Offset]Type),
		typeSigs:    make(map[uint64]*typeUnit),
		hashes:      make(map[string]*Hash),
		infoSec:     &remoteSection{r: info.Reader, size: info.Size, name: "info"},
		strSec:      &remoteSection{r: str.Reader, size: str.Size, name: "str"},
	}
	if err := d.init(); err != nil {
		return nil, err
	}
	return d, nil
}

// init sniffs the byte order from .debug_info, then builds the unit
// directory. It reads only what the two tasks require.
func (d *Data) init() error {
	if d.infoSec != nil {
		if err := d.sniffByteOrderRemote(); err != nil {
			return err
		}
	} else {
		if err := d.sniffByteOrderSlice(); err != nil {
			return err
		}
	}
	u, err := d.parseUnits()
	if err != nil {
		return err
	}
	d.unit = u
	return nil
}

// Sniff .debug_info to figure out byte order.
// 32-bit DWARF: 4 byte length, 2 byte version.
// 64-bit DWARF: 4 bytes of 0xff, 8 byte length, 2 byte version.
func (d *Data) sniffByteOrderSlice() error {
	if len(d.info) < 6 {
		return DecodeError{Name: "info", Offset: Offset(len(d.info)), Err: "too short"}
	}
	offset := 4
	if d.info[0] == 0xff && d.info[1] == 0xff && d.info[2] == 0xff && d.info[3] == 0xff {
		if len(d.info) < 14 {
			return DecodeError{Name: "info", Offset: Offset(len(d.info)), Err: "too short"}
		}
		offset = 12
	}
	// Fetch the version, a tiny 16-bit number (1, 2, 3, 4, 5).
	x, y := d.info[offset], d.info[offset+1]
	return d.setByteOrder(x, y)
}

func (d *Data) sniffByteOrderRemote() error {
	// Read only the initial length and the version; never the whole
	// section.
	sec := d.infoSec
	if sec.size < 6 {
		return DecodeError{Name: "info", Offset: Offset(sec.size), Err: "too short"}
	}
	length := make([]byte, 4)
	if err := sec.readAt(length, 0); err != nil {
		return err
	}
	versionOff := int64(4)
	if length[0] == 0xff && length[1] == 0xff && length[2] == 0xff && length[3] == 0xff {
		// 64-bit DWARF: marker(4) + length(8) precede the version.
		versionOff = 12
		if sec.size < 14 {
			return DecodeError{Name: "info", Offset: Offset(sec.size), Err: "too short"}
		}
	}
	version := make([]byte, 2)
	if err := sec.readAt(version, versionOff); err != nil {
		return err
	}
	return d.setByteOrder(version[0], version[1])
}

func (d *Data) setByteOrder(x, y byte) error {
	switch {
	case x == 0 && y == 0:
		return DecodeError{Name: "info", Offset: 4, Err: "unsupported version 0"}
	case x == 0:
		d.bigEndian = true
		d.order = binary.BigEndian
	case y == 0:
		d.bigEndian = false
		d.order = binary.LittleEndian
	default:
		return DecodeError{Name: "info", Offset: 4, Err: "cannot determine byte order"}
	}
	return nil
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
