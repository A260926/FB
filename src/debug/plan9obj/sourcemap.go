// Copyright 2024 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package plan9obj

import (
	"encoding/binary"
	"fmt"
	"sort"
	"strings"
)

// A SourceLocation is the source position associated with a text address.
//
// Symbol is the original name of the text symbol containing the address,
// or the empty string if no text symbol contains it.
// Line is the absolute (preprocessor) line number recovered from the
// compressed pc/line table.
// File is the source file selected by the symbol history, or the empty
// string when the position precedes any history. When File is empty,
// FileLine is 0.
// FileLine is the 1-based line number within File.
type SourceLocation struct {
	Symbol   string
	Line     int64
	File     string
	FileLine int64
}

// A SourceMap maps text addresses of a Plan 9 a.out binary to source
// locations. The map owns all the data it needs, so it remains valid
// after the underlying [File] is closed or its ReaderAt is reused.
type SourceMap struct {
	textStart uint64
	textEnd   uint64
	quantum   uint8
	points    []pcPoint
	texts     []textRange
}

type pcPoint struct {
	pc   uint64
	line int64
}

type textRange struct {
	off  uint64 // symbol value, relative to the start of the text section
	name string
	hist []histEvent
}

// histEvent is one entry of a symbol history segment.
// The types are:
//   - 'r': a z record with value 1, starting a fresh compilation
//     unit history (the file stack is cleared);
//   - 'z': a push (push == true, named file) or pop (push == false);
//   - 'Z': a #line directive re-basing the current file's line number.
//
// For z/r entries, line is the absolute pc/line table line at which
// the event happens. For Z entries, line is the line number named by
// the directive.
type histEvent struct {
	line int64
	typ  byte
	name string
	push bool
}

// histFrame is one file on the history stack.
// pushAbs is the immutable absolute line at which the file was entered.
// baseAbs/baseFile define the current mapping from absolute lines to
// lines within the file (fileLine = baseFile + (abs-baseAbs) - childSpan)
// and are adjusted by #line (Z) directives.
// childSpan counts absolute lines consumed by nested inclusions.
type histFrame struct {
	name      string
	pushAbs   int64
	baseAbs   int64
	baseFile  int64
	childSpan int64
}

// sourceMapError reports a malformed section, naming both the section
// and the byte offset within it where the problem was detected.
type sourceMapError struct {
	section string
	off     int64
	msg     string
}

func (e *sourceMapError) Error() string {
	return fmt.Sprintf("plan9obj: malformed %s section: %s at offset %#x", e.section, e.msg, e.off)
}

// SourceMap decodes the compressed pc/line table (the pcsz section) and
// the symbol table history (z/Z records) of f into a queryable source
// map.
//
// textStart is the virtual address of the first byte of the text
// section; it is supplied by the caller rather than inferred from
// [FileHeader.Entry]. The covered address range is the half-open
// interval [textStart, textStart+len(text)).
//
// quantum is the instruction size in bytes used by the pc/line table
// and must be 1, 2 or 4.
//
// The required sections are read in full during construction; later
// calls to [File.Close] or changes to the underlying reader do not
// affect the returned map. An empty pcsz section yields an empty,
// non-nil map.
func (f *File) SourceMap(textStart uint64, quantum uint8) (*SourceMap, error) {
	switch quantum {
	case 1, 2, 4:
	default:
		return nil, fmt.Errorf("plan9obj: quantum must be 1, 2 or 4, got %d", quantum)
	}

	text := f.Section("text")
	if text == nil {
		return nil, fmt.Errorf("plan9obj: missing text section")
	}
	textEnd, overflow := addU64(textStart, uint64(text.Size))
	if overflow {
		return nil, fmt.Errorf("plan9obj: text range [textStart, textStart+%#x) overflows", text.Size)
	}
	if textStart < uint64(quantum) {
		return nil, fmt.Errorf("plan9obj: textStart %#x is below one quantum of %d", textStart, quantum)
	}

	pcsz := f.Section("pcsz")
	if pcsz == nil {
		return nil, fmt.Errorf("plan9obj: missing pcsz section")
	}
	pcdata, err := pcsz.Data()
	if err != nil {
		return nil, fmt.Errorf("plan9obj: cannot load pcsz section: %w", err)
	}

	var symdata []byte
	if syms := f.Section("syms"); syms != nil {
		symdata, err = syms.Data()
		if err != nil {
			return nil, fmt.Errorf("plan9obj: cannot load syms section: %w", err)
		}
	}

	points, err := decodePcln(pcdata, textStart, uint64(quantum))
	if err != nil {
		return nil, err
	}
	texts, err := parseTextHistories(symdata, f.PtrSize)
	if err != nil {
		return nil, err
	}

	return &SourceMap{
		textStart: textStart,
		textEnd:   textEnd,
		quantum:   quantum,
		points:    points,
		texts:     texts,
	}, nil
}

// decodePcln expands the compressed pc/line byte stream.
//
// The initial program counter is textStart-quantum and the initial line
// is 0. Each byte adjusts the line and/or the counter; every record
// then advances the counter by one quantum. The returned points are the
// (pc, line) states reached after each record, in increasing pc order.
func decodePcln(data []byte, textStart, quantum uint64) ([]pcPoint, error) {
	pc := textStart - quantum
	line := int64(0)
	points := make([]pcPoint, 0, len(data))

	for i := 0; i < len(data); i++ {
		off := int64(i)
		b := data[i]
		switch {
		case b == 0:
			// A big-endian signed 32-bit line delta follows.
			if i+4 >= len(data) {
				return nil, &sourceMapError{"pcsz", off, "truncated 32-bit line delta"}
			}
			delta := int64(int32(binary.BigEndian.Uint32(data[i+1 : i+5])))
			next, overflow := addInt64(line, delta)
			if overflow {
				return nil, &sourceMapError{"pcsz", off, "line number overflow"}
			}
			line = next
			i += 4
		case b <= 64:
			next, overflow := addInt64(line, int64(b))
			if overflow {
				return nil, &sourceMapError{"pcsz", off, "line number overflow"}
			}
			line = next
		case b <= 128:
			next, overflow := addInt64(line, -int64(b-64))
			if overflow {
				return nil, &sourceMapError{"pcsz", off, "line number overflow"}
			}
			line = next
		default:
			// Advance the counter by an additional (b-129) quanta
			// without changing the line.
			step, overflow := mulU64(uint64(b-129), quantum)
			if overflow {
				return nil, &sourceMapError{"pcsz", off, "program counter overflow"}
			}
			pc, overflow = addU64(pc, step)
			if overflow {
				return nil, &sourceMapError{"pcsz", off, "program counter overflow"}
			}
		}
		var overflow bool
		pc, overflow = addU64(pc, quantum)
		if overflow {
			return nil, &sourceMapError{"pcsz", off, "program counter overflow"}
		}
		points = append(points, pcPoint{pc, line})
	}
	return points, nil
}

// parseTextHistories walks the raw symbol table in its original order
// and binds each text symbol to the symbol history (z/Z records) in
// effect when it appears.
func parseTextHistories(data []byte, ptrsz int) ([]textRange, error) {
	files := make(map[uint64]string)
	var cur []histEvent
	depth := 0
	var texts []textRange

	p := data
	off := 0
	for len(p) >= 4 {
		recOff := int64(off)
		if len(p) < ptrsz {
			return nil, &sourceMapError{"syms", recOff, "truncated symbol value"}
		}
		var value uint64
		if ptrsz == 8 {
			value = binary.BigEndian.Uint64(p[:8])
			p = p[8:]
			off += 8
		} else {
			value = uint64(binary.BigEndian.Uint32(p[:4]))
			p = p[4:]
			off += 4
		}
		if len(p) < 1 {
			return nil, &sourceMapError{"syms", recOff, "truncated symbol type"}
		}
		typ := p[0] & 0x7f
		p = p[1:]
		off++

		// Name, NUL terminated.
		end := 0
		for end < len(p) && p[end] != 0 {
			end++
		}
		if end >= len(p) {
			return nil, &sourceMapError{"syms", recOff, "unterminated symbol name"}
		}
		name := string(p[:end])
		p = p[end+1:]
		off += end + 1

		switch typ {
		case 'f':
			files[value] = name
			continue
		case 'z', 'Z':
			// History records terminate their component list with
			// a 0x0000 pair.
			var codes []uint16
			for {
				if len(p) < 2 {
					return nil, &sourceMapError{"syms", recOff, "truncated symbol history"}
				}
				code := binary.BigEndian.Uint16(p[:2])
				p = p[2:]
				off += 2
				if code == 0 {
					break
				}
				codes = append(codes, code)
			}
			if typ == 'Z' {
				if depth == 0 {
					return nil, &sourceMapError{"syms", recOff, "orphan Z line directive"}
				}
				cur = append(cur, histEvent{line: int64(value), typ: 'Z'})
				continue
			}
			if value == 1 {
				// A z record with value 1 starts a fresh
				// compilation unit history.
				cur = []histEvent{{line: 1, typ: 'r'}}
				depth = 0
				if len(codes) == 0 {
					continue
				}
				file, err := resolveFile(codes, files, recOff)
				if err != nil {
					return nil, err
				}
				cur = append(cur, histEvent{line: 1, typ: 'z', push: true, name: file})
				depth = 1
				continue
			}
			if len(codes) == 0 {
				// Empty name: leave the current file.
				if depth == 0 {
					return nil, &sourceMapError{"syms", recOff, "symbol history stack underflow"}
				}
				cur = append(cur, histEvent{line: int64(value), typ: 'z'})
				depth--
				continue
			}
			file, err := resolveFile(codes, files, recOff)
			if err != nil {
				return nil, err
			}
			cur = append(cur, histEvent{line: int64(value), typ: 'z', push: true, name: file})
			depth++
		case 'T', 't', 'L', 'l':
			texts = append(texts, textRange{
				off:  value,
				name: name,
				hist: append([]histEvent(nil), cur...),
			})
		}
	}

	// Text symbols are indexed by text-relative offset; ties keep the
	// symbol that appeared first in the table.
	sort.SliceStable(texts, func(i, j int) bool { return texts[i].off < texts[j].off })
	uniq := texts[:0]
	for _, tr := range texts {
		if len(uniq) > 0 && uniq[len(uniq)-1].off == tr.off {
			continue
		}
		uniq = append(uniq, tr)
	}
	return uniq, nil
}

func resolveFile(codes []uint16, files map[uint64]string, recOff int64) (string, error) {
	var b strings.Builder
	for _, code := range codes {
		file, ok := files[uint64(code)]
		if !ok {
			return "", &sourceMapError{"syms", recOff, fmt.Sprintf("undefined file number %d", code)}
		}
		if b.Len() > 0 && b.String()[b.Len()-1] != '/' {
			b.WriteByte('/')
		}
		b.WriteString(file)
	}
	return b.String(), nil
}

// Lookup reports the source location for pc.
//
// The line is the one in effect when the decoded table first reaches a
// program counter at or beyond pc; a counter jump therefore covers all
// addresses skipped by the jump. Addresses outside the half-open text
// range, and addresses the table never reaches, return ok == false.
func (m *SourceMap) Lookup(pc uint64) (SourceLocation, bool) {
	if pc < m.textStart || pc >= m.textEnd {
		return SourceLocation{}, false
	}
	idx := sort.Search(len(m.points), func(i int) bool { return m.points[i].pc >= pc })
	if idx >= len(m.points) {
		// The table ended before it covered pc: the last line must
		// not be extended past the table.
		return SourceLocation{}, false
	}
	loc := SourceLocation{Line: m.points[idx].line}

	textOff := pc - m.textStart
	if ti := sort.Search(len(m.texts), func(i int) bool { return m.texts[i].off > textOff }) - 1; ti >= 0 {
		tr := &m.texts[ti]
		loc.Symbol = tr.name
		if file, fileLine, ok := replayHist(tr.hist, loc.Line); ok {
			loc.File = file
			loc.FileLine = fileLine
		}
	}
	return loc, true
}

// replayHist replays one symbol's history segment up to absolute line
// line and returns the current file and the line within it. Before any
// file has been entered, ok is false and the caller leaves the file
// position unknown.
func replayHist(events []histEvent, line int64) (file string, fileLine int64, ok bool) {
	var stack []histFrame
	curAbs := int64(0)
	for _, ev := range events {
		// Z values are file-relative #line numbers rather than
		// absolute lines, so only z/r events gate the replay.
		if ev.typ != 'Z' && ev.line > line {
			break
		}
		switch ev.typ {
		case 'r':
			// Start of a compilation unit: clear the stack.
			stack = stack[:0]
			curAbs = ev.line
		case 'z':
			curAbs = ev.line
			if ev.push {
				stack = append(stack, histFrame{
					name:     ev.name,
					pushAbs:  ev.line,
					baseAbs:  ev.line,
					baseFile: 1,
				})
				continue
			}
			if len(stack) == 0 {
				return "", 0, false
			}
			top := stack[len(stack)-1]
			stack = stack[:len(stack)-1]
			if len(stack) > 0 {
				// The lines consumed by the inclusion do not
				// exist in the parent file.
				stack[len(stack)-1].childSpan += ev.line - top.pushAbs
			}
		case 'Z':
			if len(stack) == 0 {
				return "", 0, false
			}
			// Re-base the current file at the current absolute
			// line: later positions use the specified file line.
			top := &stack[len(stack)-1]
			top.baseAbs = curAbs
			top.baseFile = ev.line
		}
	}
	if len(stack) == 0 {
		return "", 0, false
	}
	top := stack[len(stack)-1]
	return top.name, top.baseFile + (line - top.baseAbs) - top.childSpan, true
}

func addU64(a, b uint64) (uint64, bool) {
	s := a + b
	if s < a {
		return 0, true
	}
	return s, false
}

func mulU64(a, b uint64) (uint64, bool) {
	if a != 0 && b > (1<<64-1)/a {
		return 0, true
	}
	return a * b, false
}

func addInt64(a, b int64) (int64, bool) {
	s := a + b
	if (b > 0 && s < a) || (b < 0 && s > a) {
		return 0, true
	}
	return s, false
}
