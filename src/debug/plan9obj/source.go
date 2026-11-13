// Copyright 2014 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package plan9obj

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"
)

// A SourceLocation is the source position associated with a program counter.
type SourceLocation struct {
	// TextSymbol is the name of the text symbol (a T, t, L or l symbol)
	// whose range contains the program counter. It is empty when no text
	// symbol precedes the address.
	TextSymbol string

	// Line is the absolute line number decoded from the pc/line table.
	Line int64

	// File is the source file reconstructed from the symbol-table
	// history. It is empty when the position is not covered by any
	// file history.
	File string

	// FileLine is the 1-based line number within File. It is 0 when File
	// is unknown.
	FileLine int64
}

// A SourceMap maps program counters in a text section to source locations.
//
// The map owns the data it needs; it remains valid after the underlying
// [File] is closed or the ReaderAt passed to [NewFile] is reused.
type SourceMap struct {
	textStart uint64
	textEnd   uint64

	// Sorted program-counter entries decoded from the pcsz section.
	pcs []pcEntry

	// Compile units, in symbol-table order; each owns its file history.
	units []compileUnit

	// Text symbols sorted by address, with same-address aliases reduced
	// to the symbol that appeared first in the table.
	texts []textRef
}

type pcEntry struct {
	pc   uint64
	line int64
}

type compileUnit struct {
	events []histEvent
}

// histEvent marks that, from absolute line abs onward, source positions
// belong to file name at file line (absoluteLine - off). An empty name marks
// a region with no known file. The offset is snapshotted here because the
// same file can be left and re-entered with a different offset.
type histEvent struct {
	abs  int64
	name string
	off  int64
}

// histFrame is the transient include stack used while parsing a unit.
type histFrame struct {
	name  string
	abs   int64
	off   int64 // current file-line offset for this frame's active span
	pause int64 // parent file line where this include started
}

type textRef struct {
	addr uint64
	name string
	unit int // index into SourceMap.units, -1 when outside any unit
}

// SourceMap decodes the pc/line table (the pcsz section) together with the
// symbol-table file history and returns a queryable source map.
//
// textStart is the virtual address of the first byte of the text section;
// the covered range is the half-open interval [textStart, textStart+len(text)).
// It is supplied explicitly because [FileHeader.Entry] is not necessarily the
// start of the text section. quantum is the instruction size in bytes and
// must be 1, 2 or 4.
//
// No external tools are invoked and no source files are read.
func (f *File) SourceMap(textStart uint64, quantum uint8) (*SourceMap, error) {
	switch quantum {
	case 1, 2, 4:
	default:
		return nil, fmt.Errorf("plan9obj: quantum must be 1, 2 or 4, got %d", quantum)
	}

	textSec := f.Section("text")
	if textSec == nil {
		return nil, fmt.Errorf("plan9obj: file has no text section")
	}
	textSize := uint64(textSec.Size)
	textEnd, ok := addU64(textStart, textSize)
	if !ok {
		return nil, &sectionError{section: "text", off: 0, msg: "text address range overflows uint64"}
	}

	pcsSec := f.Section("pcsz")
	if pcsSec == nil {
		return nil, fmt.Errorf("plan9obj: file has no pcsz section")
	}
	pcsData, err := pcsSec.Data()
	if err != nil {
		return nil, fmt.Errorf("plan9obj: cannot load pcsz section: %w", err)
	}

	m := &SourceMap{
		textStart: textStart,
		textEnd:   textEnd,
	}

	// An empty pc/line table yields an empty map; there is nothing to
	// decode and no history can ever be queried.
	if len(pcsData) == 0 {
		return m, nil
	}

	m.pcs, err = decodeLineTable(pcsData, textStart, quantum)
	if err != nil {
		return nil, err
	}

	if symsSec := f.Section("syms"); symsSec != nil && symsSec.Size > 0 {
		symsData, err := symsSec.Data()
		if err != nil {
			return nil, fmt.Errorf("plan9obj: cannot load syms section: %w", err)
		}
		if err := m.parseHistory(symsData, f.PtrSize, textStart); err != nil {
			return nil, err
		}
	}

	return m, nil
}

// sectionError reports a malformed section, naming the section and the
// byte offset within the section where the problem was found.
type sectionError struct {
	section string
	off     int64
	msg     string
}

func (e *sectionError) Error() string {
	return fmt.Sprintf("plan9obj: malformed %s section at offset %#x: %s", e.section, e.off, e.msg)
}

// decodeLineTable decodes the compressed Plan 9 pc/line stream.
//
// The running program counter starts one quantum before textStart and the
// line number at zero. Every record advances the program counter by one
// quantum; records with byte values 129..255 additionally skip that many
// extra quanta without changing the line.
func decodeLineTable(data []byte, textStart uint64, quantum uint8) ([]pcEntry, error) {
	q := uint64(quantum)
	if textStart < q {
		return nil, &sectionError{
			section: "pcsz",
			off:     0,
			msg:     "initial program counter underflows before textStart",
		}
	}
	pc := textStart - q

	out := make([]pcEntry, 0, len(data))
	var line int64

	for i := 0; i < len(data); {
		recOff := int64(i)
		b := data[i]
		i++

		var jump int64 // extra quanta, byte values 129..255
		var delta int64
		switch {
		case b == 0:
			if i+4 > len(data) {
				return nil, &sectionError{
					section: "pcsz",
					off:     recOff,
					msg:     "truncated 32-bit line increment",
				}
			}
			delta = int64(int32(binary.BigEndian.Uint32(data[i : i+4])))
			i += 4
		case b >= 1 && b <= 64:
			delta = int64(b)
		case b >= 65 && b <= 128:
			delta = -int64(b - 64)
		default: // 129..255
			jump = int64(b - 129)
		}

		newLine, ok := addI64(line, delta)
		if !ok {
			return nil, &sectionError{
				section: "pcsz",
				off:     recOff,
				msg:     "line number arithmetic overflows int64",
			}
		}
		line = newLine

		// Each record moves at least one quantum; jump records move
		// (b-129) additional quanta.
		adv := uint64(jump+1) * q
		newPC, ok := addU64(pc, adv)
		if !ok {
			return nil, &sectionError{
				section: "pcsz",
				off:     recOff,
				msg:     "program counter arithmetic overflows uint64",
			}
		}
		pc = newPC

		out = append(out, pcEntry{pc: pc, line: line})
	}

	return out, nil
}

// parseHistory walks the symbol table and reconstructs, per compile unit,
// the include/#line history and the text symbols belonging to the unit.
func (m *SourceMap) parseHistory(data []byte, ptrsz int, textStart uint64) error {
	files := make(map[uint16]string)

	// Current compile-unit state while walking the table.
	var stack []*histFrame
	curUnit := -1

	var texts []textRef

	off := 0
	for off+ptrsz+1 <= len(data) {
		recOff := int64(off)

		var value uint64
		if ptrsz == 8 {
			value = binary.BigEndian.Uint64(data[off : off+8])
		} else {
			value = uint64(binary.BigEndian.Uint32(data[off : off+4]))
		}
		off += ptrsz

		typ := data[off] & 0x7F
		off++

		// Name bytes: NUL-terminated strings for ordinary symbols; for
		// z/Z a leading NUL followed by big-endian 16-bit file-number
		// pairs terminated by a double NUL.
		var name []byte
		if typ == 'z' || typ == 'Z' {
			if off >= len(data) || data[off] != 0 {
				return &sectionError{section: "syms", off: recOff, msg: "malformed history record name"}
			}
			off++ // leading NUL
			pairs := off
			for {
				if off+2 > len(data) {
					return &sectionError{section: "syms", off: recOff, msg: "truncated history file-number list"}
				}
				if data[off] == 0 && data[off+1] == 0 {
					name = data[pairs:off]
					off += 2
					break
				}
				off += 2
			}
		} else {
			start := off
			for off < len(data) && data[off] != 0 {
				off++
			}
			if off >= len(data) {
				return &sectionError{section: "syms", off: recOff, msg: "truncated symbol name"}
			}
			name = data[start:off]
			off++ // terminating NUL
		}

		switch typ {
		case 'f':
			if value > math.MaxUint16 {
				return &sectionError{section: "syms", off: recOff, msg: "file number out of uint16 range"}
			}
			files[uint16(value)] = string(name)

		case 'T', 't', 'L', 'l':
			addr, ok := addU64(textStart, value)
			if !ok {
				return &sectionError{section: "syms", off: recOff, msg: "text symbol address overflows uint64"}
			}
			texts = append(texts, textRef{
				addr: addr,
				name: string(name),
				unit: curUnit,
			})

		case 'z', 'Z':
			if value > math.MaxInt64 {
				return &sectionError{section: "syms", off: recOff, msg: "history line value overflows int64"}
			}
			abs := int64(value)

			if typ == 'z' && abs == 1 {
				// A value of 1 starts the history of a new compile unit.
				m.units = append(m.units, compileUnit{})
				curUnit = len(m.units) - 1
				stack = stack[:0]
			}
			if curUnit < 0 {
				return &sectionError{section: "syms", off: recOff, msg: "history record before first compile unit"}
			}
			unit := &m.units[curUnit]

			if typ == 'Z' {
				// Z carries a #line directive's target file line number. It
				// re-numbers the preceding (active) history entry: the
				// absolute line at which that frame was entered now maps to
				// the given file line. A corrective snapshot at the frame's
				// entry absolute line supersedes its original offset.
				if len(stack) == 0 {
					return &sectionError{section: "syms", off: recOff, msg: "Z record without an active history entry"}
				}
				top := stack[len(stack)-1]
				lineOff, ok := subI64(top.abs, int64(value))
				if !ok {
					return &sectionError{section: "syms", off: recOff, msg: "#line offset arithmetic overflows int64"}
				}
				top.off = lineOff
				unit.events = append(unit.events, histEvent{abs: top.abs, name: top.name, off: lineOff})
				continue
			}

			// Resolve the file-number components into a path.
			path, err := decodeHistoryPath(name, files, recOff)
			if err != nil {
				return err
			}

			if path != "" {
				// A non-empty name enters a file.
				frameOff, ok := subI64(abs, 1)
				if !ok {
					return &sectionError{section: "syms", off: recOff, msg: "history line arithmetic overflows int64"}
				}
				frame := &histFrame{name: path, abs: abs, off: frameOff}
				if len(stack) > 0 {
					parent := stack[len(stack)-1]
					pause, ok := subI64(abs, parent.off)
					if !ok {
						return &sectionError{section: "syms", off: recOff, msg: "include line arithmetic overflows int64"}
					}
					frame.pause = pause
				}
				stack = append(stack, frame)
				unit.events = append(unit.events, histEvent{abs: abs, name: path, off: frameOff})
			} else {
				// An empty name returns to the parent file. The absolute
				// lines consumed by the include are deducted so the parent
				// resumes on the line after where it left off.
				if len(stack) == 0 {
					return &sectionError{section: "syms", off: recOff, msg: "history stack underflow on empty file name"}
				}
				child := stack[len(stack)-1]
				stack = stack[:len(stack)-1]
				if len(stack) > 0 {
					parent := stack[len(stack)-1]
					resume, ok := addI64(child.pause, 1)
					if !ok {
						return &sectionError{section: "syms", off: recOff, msg: "include line arithmetic overflows int64"}
					}
					newOff, ok := subI64(abs, resume)
					if !ok {
						return &sectionError{section: "syms", off: recOff, msg: "include line arithmetic overflows int64"}
					}
					parent.off = newOff
					unit.events = append(unit.events, histEvent{abs: abs, name: parent.name, off: newOff})
				} else {
					unit.events = append(unit.events, histEvent{abs: abs})
				}
			}
		}
	}
	if off != len(data) {
		return &sectionError{
			section: "syms",
			off:     int64(off),
			msg:     "trailing bytes do not form a complete symbol record",
		}
	}

	// A #line snapshot is keyed at its frame's entry absolute line, so it
	// can precede later events in the assembled slice. Sort each unit's
	// history by absolute line; a stable sort keeps the later snapshot
	// ahead of the original entry on a tie, which is the intended override.
	for u := range m.units {
		events := m.units[u].events
		sort.SliceStable(events, func(i, j int) bool { return events[i].abs < events[j].abs })
	}

	// Stable sort keeps the first symbol at a duplicated address; the
	// subsequent pass drops later aliases of the same address.
	sort.SliceStable(texts, func(i, j int) bool { return texts[i].addr < texts[j].addr })
	m.texts = m.texts[:0]
	for _, ts := range texts {
		if n := len(m.texts); n > 0 && m.texts[n-1].addr == ts.addr {
			continue
		}
		m.texts = append(m.texts, ts)
	}

	return nil
}

func decodeHistoryPath(pairs []byte, files map[uint16]string, recOff int64) (string, error) {
	var path string
	for i := 0; i+2 <= len(pairs); i += 2 {
		idx := binary.BigEndian.Uint16(pairs[i : i+2])
		elt, ok := files[idx]
		if !ok {
			return "", &sectionError{
				section: "syms",
				off:     recOff,
				msg:     fmt.Sprintf("undefined file number %d", idx),
			}
		}
		if len(path) > 0 && path[len(path)-1] != '/' {
			path += "/"
		}
		path += elt
	}
	return path, nil
}

// Lookup returns the source location for pc.
//
// The line number is the one recorded by the first pc/line entry whose
// program counter reaches or exceeds pc. Addresses the table never reaches,
// including everything past its final entry, have no result rather than
// inheriting the last line.
func (m *SourceMap) Lookup(pc uint64) (SourceLocation, bool) {
	if len(m.pcs) == 0 || pc < m.textStart || pc >= m.textEnd {
		return SourceLocation{}, false
	}

	// First entry reaching or exceeding pc.
	i := sort.Search(len(m.pcs), func(i int) bool { return m.pcs[i].pc >= pc })
	if i >= len(m.pcs) {
		return SourceLocation{}, false
	}
	line := m.pcs[i].line
	loc := SourceLocation{Line: line}

	// Containing text symbol: the last one starting at or before pc.
	if len(m.texts) > 0 {
		j := sort.Search(len(m.texts), func(j int) bool { return m.texts[j].addr > pc }) - 1
		if j >= 0 {
			ts := m.texts[j]
			loc.TextSymbol = ts.name
			if ts.unit >= 0 && ts.unit < len(m.units) {
				events := m.units[ts.unit].events
				k := sort.Search(len(events), func(k int) bool { return events[k].abs > line }) - 1
				if k >= 0 && events[k].name != "" {
					ev := events[k]
					fileLine, ok := subI64(line, ev.off)
					if ok {
						loc.File = ev.name
						loc.FileLine = fileLine
					}
				}
			}
		}
	}

	return loc, true
}

func addU64(a, b uint64) (uint64, bool) {
	s := a + b
	if s < a {
		return 0, false
	}
	return s, true
}

func addI64(a, b int64) (int64, bool) {
	s := a + b
	if (b > 0 && s < a) || (b < 0 && s > a) {
		return 0, false
	}
	return s, true
}

func subI64(a, b int64) (int64, bool) {
	d := a - b
	if (b > 0 && d > a) || (b < 0 && d < a) {
		return 0, false
	}
	return d, true
}
