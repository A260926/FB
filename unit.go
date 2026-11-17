// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package dwarf

import (
	"sort"
	"strconv"
)

// DWARF debug info is split into a sequence of compilation units.
// Each unit has its own abbreviation table and address size.

type unit struct {
	base Offset // byte offset of header within the aggregate info
	off  Offset // byte offset of data within the aggregate info
	end  Offset // byte offset just past the end of the unit

	// data is the unit body for slice-backed Data. Source-backed
	// Data leaves this nil and fetches bytes through the info source.
	data   []byte
	atable abbrevTable
	*unit5 // info specific to DWARF 5 units
	asize  int
	vers   int
	is64   bool  // True for 64-bit DWARF format
	utype  uint8 // DWARF 5 unit type
}

type unit5 struct {
	addrBase       uint64
	strOffsetsBase uint64
	rngListsBase   uint64
	locListsBase   uint64
}

// Implement the dataFormat interface.

func (u *unit) version() int {
	return u.vers
}

func (u *unit) dwarf64() (bool, bool) {
	return u.is64, true
}

func (u *unit) addrsize() int {
	return u.asize
}

func (u *unit) addrBase() uint64 {
	if u.unit5 != nil {
		return u.unit5.addrBase
	}
	return 0
}

func (u *unit) strOffsetsBase() uint64 {
	if u.unit5 != nil {
		return u.unit5.strOffsetsBase
	}
	return 0
}

func (u *unit) rngListsBase() uint64 {
	if u.unit5 != nil {
		return u.unit5.rngListsBase
	}
	return 0
}

func (u *unit) locListsBase() uint64 {
	if u.unit5 != nil {
		return u.unit5.locListsBase
	}
	return 0
}

// infoBuf returns a buf over the unit's info section starting at off.
// It uses the unit's byte slice when present, otherwise it reads from
// the shared info source and clips reads to the unit boundary.
func (d *Data) infoBuf(u *unit, format dataFormat, off Offset) buf {
	src := d.infoSource()
	if src == nil {
		return makeBuf(d, format, "info", off, u.data[off-u.off:])
	}
	return makeSrcBufLimited(d, format, "info", off, u.end, src)
}

// parseUnits builds the unit directory. Only unit headers are read:
// for source-backed data the DIE bodies are never fetched here, so a
// unit containing a huge constant block still costs just its header and
// the following unit remains directly addressable.
func (d *Data) parseUnits() ([]unit, error) {
	var units []unit

	src := d.infoSource()
	var b buf
	if src != nil {
		// Exact-sized reads: only the headers needed to build the
		// unit directory are fetched, never DIE bodies.
		b = makeSrcBufExact(d, unknownFormat{}, "info", 0, src)
	} else {
		b = makeBuf(d, unknownFormat{}, "info", 0, d.info)
	}

	for {
		// A zero-length unit terminates the section; a reader at the
		// end of the section is done too.
		if b.err != nil {
			return nil, b.err
		}
		if src == nil && len(b.data) == 0 {
			break
		}
		if src != nil && b.off >= Offset(src.size) {
			break
		}

		u := unit{}
		u.base = b.off
		n, is64 := b.unitLength()
		if b.err != nil {
			return nil, b.err
		}
		if n == 0 {
			// Zero-length padding terminates the section.
			break
		}
		u.is64 = is64
		// The unit length does not count the initial-length field
		// itself (4 bytes, or 12 bytes in DWARF64).
		headLen := Offset(4)
		if is64 {
			headLen = 12
		}
		u.end = u.base + headLen + n

		vers := b.uint16()
		if vers < 2 || vers > 5 {
			b.error("unsupported DWARF version " + strconv.Itoa(int(vers)))
			return nil, b.err
		}
		u.vers = int(vers)
		if vers >= 5 {
			u.utype = b.uint8()
			u.asize = int(b.uint8())
		}
		var abbrevOff uint64
		if u.is64 {
			abbrevOff = b.uint64()
		} else {
			abbrevOff = uint64(b.uint32())
		}
		atable, err := d.parseAbbrev(abbrevOff, u.vers)
		if err != nil {
			if b.err == nil {
				return nil, err
			}
			return nil, b.err
		}
		u.atable = atable
		if vers < 5 {
			u.asize = int(b.uint8())
		}

		switch u.utype {
		case utSkeleton, utSplitCompile:
			b.uint64() // unit ID
		case utType, utSplitType:
			b.uint64()  // type signature
			if u.is64 { // type offset
				b.uint64()
			} else {
				b.uint32()
			}
		}
		if b.err != nil {
			return nil, b.err
		}

		u.off = b.off
		if src == nil {
			if u.end > Offset(len(d.info)) {
				return nil, DecodeError{"info", u.base, "unit length exceeds section size", nil}
			}
			u.data = d.info[u.off:u.end]
			// Position the slice buf at the start of the next unit.
			b.data = d.info[u.end:]
			b.off = u.end
		} else {
			if u.end > Offset(src.size) {
				return nil, DecodeError{"info", u.base, "unit length exceeds section size", nil}
			}
			// Jump straight to the next header without touching the
			// body bytes in between.
			b.off = u.end
			b.data = b.data[:0]
		}
		units = append(units, u)
	}
	return units, nil
}

// offsetToUnit returns the index of the unit containing offset off.
// It returns -1 if no unit contains this offset.
func (d *Data) offsetToUnit(off Offset) int {
	// Find the unit after off
	next := sort.Search(len(d.unit), func(i int) bool {
		return d.unit[i].off > off
	})
	if next == 0 {
		return -1
	}
	u := &d.unit[next-1]
	if u.off <= off && off < u.end {
		return next - 1
	}
	return -1
}

func (d *Data) collectDwarf5BaseOffsets(u *unit) error {
	if u.unit5 == nil {
		panic("expected unit5 to be set up already")
	}
	b := d.infoBuf(u, u, u.off)
	cu := b.entry(nil, u)
	if cu == nil {
		// Unknown abbreviation table entry or some other fatal
		// problem; bail early on the assumption that this will be
		// detected at some later point.
		return b.err
	}
	if iAddrBase, ok := cu.Val(AttrAddrBase).(int64); ok {
		u.unit5.addrBase = uint64(iAddrBase)
	}
	if iStrOffsetsBase, ok := cu.Val(AttrStrOffsetsBase).(int64); ok {
		u.unit5.strOffsetsBase = uint64(iStrOffsetsBase)
	}
	if iRngListsBase, ok := cu.Val(AttrRnglistsBase).(int64); ok {
		u.unit5.rngListsBase = uint64(iRngListsBase)
	}
	if iLocListsBase, ok := cu.Val(AttrLoclistsBase).(int64); ok {
		u.unit5.locListsBase = uint64(iLocListsBase)
	}
	return nil
}
