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
	end  Offset // byte offset just past the last DIE byte of the unit
	// A unit is either backed by a fully resident slice (data) or a
	// ReaderAt-backed section (sec). Exactly one of the two is set.
	data   []byte
	sec    *remoteSection
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

// unitBuf returns a buf positioned at off inside unit u. Slice-backed
// units reslice their bytes; ReaderAt-backed units open an independent
// window, so both entry points share the same DIE parsing logic.
func (d *Data) unitBuf(u *unit, off Offset) buf {
	if u.sec != nil {
		return d.makeRemoteBuf(u, "info", off, u.sec)
	}
	return makeBuf(d, u, "info", off, u.data[off-u.off:])
}

// parseUnits scans .debug_info building the unit directory. Only unit
// headers are read: DIE bodies are never touched here, and for
// ReaderAt-backed sections even units containing enormous constant
// blocks are skipped by offset arithmetic alone.
func (d *Data) parseUnits() ([]unit, error) {
	if d.infoSec != nil {
		b := d.makeRemoteBuf(unknownFormat{}, "info", 0, d.infoSec)
		return d.scanUnits(&b, d.infoSec.size)
	}
	b := makeBuf(d, unknownFormat{}, "info", 0, d.info)
	return d.scanUnits(&b, int64(len(d.info)))
}

func (d *Data) scanUnits(b *buf, sectionSize int64) ([]unit, error) {
	var units []unit
	for int64(b.off) < sectionSize && b.err == nil {
		u := unit{}
		u.base = b.off
		n, is64 := b.unitLength()
		if b.err != nil {
			return nil, b.err
		}
		if n == 0 {
			// A zero unit length terminates the section.
			break
		}
		dataOff := b.off
		u.is64 = is64
		vers := b.uint16()
		if vers < 2 || vers > 5 {
			b.error("unsupported DWARF version " + strconv.Itoa(int(vers)))
			break
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
		if b.err != nil {
			break
		}
		atable, err := d.parseAbbrev(abbrevOff, u.vers)
		if err != nil {
			if b.err == nil {
				b.err = err
			}
			break
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
			break
		}

		u.off = b.off
		u.end = dataOff + n
		if u.end > Offset(sectionSize) || u.end < u.off {
			b.error("unit length extends past end of info section")
			break
		}
		if b.remote != nil {
			// Do not read the DIE body. Drop the window and continue
			// at the next unit header; large blocks cost nothing.
			u.sec = b.remote.sec
			b.off = u.end
			b.data = nil
			b.remote.win = nil
		} else {
			bodyLen := int(u.end - u.off)
			u.data = b.bytes(bodyLen)
		}
		units = append(units, u)
	}
	if b.err != nil {
		return nil, b.err
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
	b := d.unitBuf(u, u.off)
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
