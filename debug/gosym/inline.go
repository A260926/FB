// Copyright 2024 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

/*Copyright (C) 2024 Mandiant, Inc. All Rights Reserved.*/

package gosym

import (
	"fmt"
)

// InlineFrame describes one level of an inlining chain at an instruction PC.
// A slice returned from an inline query is ordered from the innermost inlined
// callee out to the physical (out-of-line) function.
type InlineFrame struct {
	Name      string // function name, taken from the pclntab funcnametab
	File      string // source file describing this frame's position
	Line      int    // source line of this frame's call position
	StartLine int    // line at which the function was declared
	PC        uint64 // instruction PC for this frame (the query PC for the physical frame)
	Inlined   bool   // true for inlined frames, false for the physical function frame
}

// SupportsInlineTree reports whether the line table uses the Go 1.18+
// entry-offset _func layout that carries the inline tree PCDATA/FUNCDATA
// slots consumed by inline recovery.
func (t *LineTable) SupportsInlineTree() bool {
	t.parsePclnTab("")
	return t.Version >= ver118 && t.Ptrsize == 8
}

// FuncData is an exported view of a runtime._func record.
type FuncData struct {
	t    *LineTable
	data []byte
}

// FindFuncData locates the _func record whose function contains pc, or returns
// nil when pc falls outside every function known to the line table.
func (t *LineTable) FindFuncData(pc uint64) *FuncData {
	t.parsePclnTab("")
	fd := t.findFunc(pc)
	if fd.IsZero() {
		return nil
	}
	return &FuncData{t: t, data: fd.data}
}

// EntryPC is the entry instruction address of the function.
func (f *FuncData) EntryPC() uint64 {
	fd := funcData{t: f.t, data: f.data}
	return fd.entryPC()
}

// StartLine returns the function declaration line stored in the _func record.
func (f *FuncData) StartLine() int32 {
	if len(f.data) < 40 {
		return 0
	}
	return int32(f.t.Binary.Uint32(f.data[36:]))
}

// NameOff returns the funcnametab offset of the function name.
func (f *FuncData) NameOff() uint32 {
	fd := funcData{t: f.t, data: f.data}
	return fd.nameoff()
}

// PCFile returns the pctab offset of the pc->file-number table.
func (f *FuncData) PCFile() uint32 {
	fd := funcData{t: f.t, data: f.data}
	return fd.pcfile()
}

// PCLn returns the pctab offset of the pc->line table.
func (f *FuncData) PCLn() uint32 {
	fd := funcData{t: f.t, data: f.data}
	return fd.pcln()
}

// CUOffset returns the offset (in cutab entries) of the function's
// compilation unit file mapping.
func (f *FuncData) CUOffset() uint32 {
	fd := funcData{t: f.t, data: f.data}
	return fd.cuOffset()
}

// NPCData returns the number of PCDATA slots following the fixed _func prefix.
func (f *FuncData) NPCData() uint32 {
	if len(f.data) < 32 {
		return 0
	}
	return f.t.Binary.Uint32(f.data[28:])
}

// NFuncData returns the number of FUNCDATA slots following the PCDATA slots.
func (f *FuncData) NFuncData() uint32 {
	if len(f.data) < 44 {
		return 0
	}
	return uint32(f.data[43])
}

// PCDataSlot returns the pctab offset of PCDATA slot i. The boolean is false
// when the slot does not exist or its offset is zero, which the runtime uses
// to mark an absent PCDATA table.
func (f *FuncData) PCDataSlot(i uint32) (uint32, bool) {
	if i >= f.NPCData() {
		return 0, false
	}
	off := 44 + i*4
	if uint32(len(f.data)) < off+4 {
		return 0, false
	}
	v := f.t.Binary.Uint32(f.data[off:])
	if v == 0 {
		return 0, false
	}
	return v, true
}

// FuncDataSlot returns the FUNCDATA offset of slot i, relative to
// moduledata.gofunc. The boolean is false when the slot does not exist or the
// slot holds the 0xffffffff absent marker. An offset of zero is valid and
// returned with true.
func (f *FuncData) FuncDataSlot(i uint32) (uint32, bool) {
	nfunc := f.NFuncData()
	if i >= nfunc {
		return 0, false
	}
	off := 44 + f.NPCData()*4 + i*4
	if uint32(len(f.data)) < off+4 {
		return 0, false
	}
	v := f.t.Binary.Uint32(f.data[off:])
	if v == ^uint32(0) {
		return 0, false
	}
	return v, true
}

// PCValue evaluates the pctab table at off for targetpc, where entry is the
// entry PC of the function owning the table. A cleanly terminating table that
// runs out before targetpc reports -1 with ok=true, matching the runtime's
// pcdatavalue behavior (no value means -1). A malformed or truncated varint
// stream, or a bad offset, reports ok=false.
func (t *LineTable) PCValue(off uint32, entry, targetpc uint64) (result int32, ok bool) {
	t.parsePclnTab("")
	if off == 0 || uint64(off) >= uint64(len(t.pctab)) {
		return 0, false
	}
	p := t.pctab[off:]
	val := int32(-1)
	pc := entry
	first := true
	for {
		// Bounded reimplementation of LineTable.step: a truncated varint
		// stream must surface as a failed lookup rather than a panic.
		uvdelta, advanced, good := readBoundedVarint(p)
		if !good {
			return 0, false
		}
		p = p[advanced:]
		if uvdelta == 0 && !first {
			// Reached the table terminator without covering targetpc.
			return -1, true
		}
		if uvdelta&1 != 0 {
			uvdelta = ^(uvdelta >> 1)
		} else {
			uvdelta >>= 1
		}
		vdelta := int32(uvdelta)
		pcdelta, advanced, good := readBoundedVarint(p)
		if !good {
			return 0, false
		}
		p = p[advanced:]
		pc += uint64(pcdelta) * uint64(t.Quantum)
		val += vdelta
		if targetpc < pc {
			return val, true
		}
		first = false
	}
}

// readBoundedVarint decodes one pctab varint without indexing past the buffer.
func readBoundedVarint(p []byte) (value uint32, consumed int, ok bool) {
	var v, shift uint32
	for shift = 0; ; shift += 7 {
		if consumed >= len(p) || shift >= 35 {
			return 0, 0, false
		}
		b := p[consumed]
		consumed++
		v |= uint32(b&0x7f) << shift
		if b&0x80 == 0 {
			return v, consumed, true
		}
	}
}

// FileLine resolves a file name and line number for pc using the physical
// function's pcfile/pcln tables and the compilation unit file mapping. The
// same physical tables describe positions inside inlined callees.
func (t *LineTable) FileLine(fd *FuncData, pc uint64) (string, int32, error) {
	t.parsePclnTab("")
	if fd == nil {
		return "", 0, fmt.Errorf("nil func data for pc %#x", pc)
	}
	entry := fd.EntryPC()
	fileVal, ok := t.PCValue(fd.PCFile(), entry, pc)
	if !ok {
		return "", 0, fmt.Errorf("pcfile table has no value for pc %#x", pc)
	}
	line, ok := t.PCValue(fd.PCLn(), entry, pc)
	if !ok {
		return "", 0, fmt.Errorf("pcln table has no value for pc %#x", pc)
	}
	if fileVal < 0 {
		return "", 0, fmt.Errorf("invalid file index %d for pc %#x", fileVal, pc)
	}
	cu := fd.CUOffset()
	cutabIndex := (cu + uint32(fileVal)) * 4
	if uint32(len(t.cutab)) < cutabIndex+4 {
		return "", 0, fmt.Errorf("cutab index %d out of bounds", cutabIndex/4)
	}
	fnoff := t.Binary.Uint32(t.cutab[cutabIndex:])
	if fnoff == ^uint32(0) || uint32(len(t.filetab)) <= fnoff {
		return "", 0, fmt.Errorf("filetab offset %d out of bounds", fnoff)
	}
	name, ok := t.checkedStringFrom(t.filetab, fnoff)
	if !ok {
		return "", 0, fmt.Errorf("unterminated file name at filetab offset %d", fnoff)
	}
	return name, line, nil
}

// FuncNameChecked resolves a funcnametab offset, rejecting offsets that fall
// outside the table or that do not terminate in a NUL byte.
func (t *LineTable) FuncNameChecked(off uint32) (string, bool) {
	t.parsePclnTab("")
	return t.checkedStringFrom(t.funcnametab, off)
}

func (t *LineTable) checkedStringFrom(arr []byte, off uint32) (string, bool) {
	if uint32(len(arr)) <= off {
		return "", false
	}
	rest := arr[off:]
	for i := 0; i < len(rest); i++ {
		if rest[i] == 0 {
			return string(rest[:i]), true
		}
	}
	return "", false
}
