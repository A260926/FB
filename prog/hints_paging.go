// Copyright 2026 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"math"
	"slices"
)

// HintsPage returns the next page of hint mutants for the call callIndex,
// resuming from the given cursor (an empty cursor starts from the beginning).
// At most limit mutants are returned; every returned mutant passes the same
// validity checks as MutateWithHints applies. Candidates rejected by these
// checks do not occupy page slots.
//
// If more candidates remain after this page, next holds a cursor that
// continues the enumeration; next is empty once the enumeration is
// exhausted. An empty page is returned only when the enumeration is
// exhausted. The cursor is an opaque byte string that can be persisted and
// reused in another process; it is bound to the target OS/arch, the exact
// original program, callIndex, and the set of comparison operands and PCs.
// Any change to the bound inputs, as well as a truncated, corrupted, or
// unsupported-version cursor, results in an error rather than a silent
// restart.
//
// The source program, comps, and cursor are left unmodified. Every returned
// program owns an independent mutable argument tree: mutating it does not
// affect the source program, other mutants, or subsequent pages.
func (p *Prog) HintsPage(callIndex int, comps CompMap, cursor []byte, limit int) (mutants []*Prog, next []byte, err error) {
	if limit <= 0 {
		return nil, nil, fmt.Errorf("prog: HintsPage: limit must be positive, got %d", limit)
	}
	if callIndex < 0 || callIndex >= len(p.Calls) {
		return nil, nil, fmt.Errorf("prog: HintsPage: call index %d is out of range (%d calls)",
			callIndex, len(p.Calls))
	}
	st := new(hintState)
	if len(cursor) != 0 {
		from, err := p.parseHintsCursor(cursor, callIndex, comps)
		if err != nil {
			return nil, nil, err
		}
		st.from = from
	}
	work := p.Clone()
	c := work.Calls[callIndex]
	full := false
	more := false
	var resume hintPos
	foreachValidHint(work, c, comps, st, func() bool {
		if full {
			// The page is already full and this is the first valid candidate
			// of the next page. Note its position and stop the enumeration.
			resume = st.cur
			more = true
			return false
		}
		mutants = append(mutants, work.Clone())
		full = len(mutants) == limit
		return true
	})
	if more {
		next = p.marshalHintsCursor(callIndex, comps, resume)
	}
	return mutants, next, nil
}

const (
	// hintsCursorMagic is "shp" followed by the cursor format version.
	hintsCursorMagic   = uint32(0x01_70_68_73)
	hintsCursorMaxSize = 2048
	// magic(4) + crc(4) + target hash(16) + prog hash(16) + comps hash(16).
	hintsCursorFixedSize = 4 + 4 + 16 + 16 + 16
)

func hintsTargetHash(p *Prog) []byte {
	sum := sha256.Sum256([]byte(p.Target.OS + "/" + p.Target.Arch))
	return sum[:16]
}

func hintsProgHash(p *Prog) []byte {
	sum := sha256.Sum256(p.Serialize())
	return sum[:16]
}

func hintsCompsHash(comps CompMap) []byte {
	type triple struct{ op1, op2, pc uint64 }
	triples := make([]triple, 0, comps.Len())
	for op1, nested := range comps {
		for op2, pcs := range nested {
			for pc := range pcs {
				triples = append(triples, triple{op1, op2, pc})
			}
		}
	}
	// The hash must not depend on the map insertion order.
	slices.SortFunc(triples, func(a, b triple) int {
		if a.op1 != b.op1 {
			return cmpUint64(a.op1, b.op1)
		}
		if a.op2 != b.op2 {
			return cmpUint64(a.op2, b.op2)
		}
		return cmpUint64(a.pc, b.pc)
	})
	h := sha256.New()
	var buf [8]byte
	for _, tr := range triples {
		for _, v := range []uint64{tr.op1, tr.op2, tr.pc} {
			binary.LittleEndian.PutUint64(buf[:], v)
			h.Write(buf[:])
		}
	}
	return h.Sum(nil)[:16]
}

func cmpUint64(a, b uint64) int {
	switch {
	case a < b:
		return -1
	case a > b:
		return 1
	}
	return 0
}

// marshalHintsCursor serializes a resume position into a self-contained
// byte string that can be stored and reused in another process.
// It never contains the program itself, candidate lists, or decompressed
// images, and is always well below hintsCursorMaxSize.
func (p *Prog) marshalHintsCursor(callIndex int, comps CompMap, pos hintPos) []byte {
	buf := make([]byte, 0, hintsCursorFixedSize+4*binary.MaxVarintLen64)
	buf = binary.LittleEndian.AppendUint32(buf, hintsCursorMagic)
	buf = append(buf, 0, 0, 0, 0) // crc32 placeholder
	buf = append(buf, hintsTargetHash(p)...)
	buf = append(buf, hintsProgHash(p)...)
	buf = append(buf, hintsCompsHash(comps)...)
	buf = binary.AppendUvarint(buf, uint64(callIndex))
	buf = binary.AppendUvarint(buf, uint64(pos.arg))
	buf = binary.AppendUvarint(buf, uint64(pos.offset))
	buf = binary.AppendUvarint(buf, uint64(pos.replacer))
	binary.LittleEndian.PutUint32(buf[4:8], crc32.ChecksumIEEE(buf[8:]))
	if len(buf) > hintsCursorMaxSize {
		panic(fmt.Sprintf("prog: hints cursor is too large: %d", len(buf)))
	}
	return buf
}

// parseHintsCursor validates a cursor against the current target, program,
// call index, and comparison table, and returns the resume position.
func (p *Prog) parseHintsCursor(cursor []byte, callIndex int, comps CompMap) (hintPos, error) {
	var pos hintPos
	fail := func(format string, args ...any) (hintPos, error) {
		return pos, fmt.Errorf("prog: invalid hints cursor: "+format, args...)
	}
	if len(cursor) > hintsCursorMaxSize {
		return fail("size %d exceeds the maximum %d", len(cursor), hintsCursorMaxSize)
	}
	if len(cursor) < hintsCursorFixedSize {
		return fail("truncated to %d bytes", len(cursor))
	}
	if binary.LittleEndian.Uint32(cursor[0:4]) != hintsCursorMagic {
		return fail("unsupported magic or version 0x%x", binary.LittleEndian.Uint32(cursor[0:4]))
	}
	if crc32.ChecksumIEEE(cursor[8:]) != binary.LittleEndian.Uint32(cursor[4:8]) {
		return fail("checksum mismatch")
	}
	if !bytes.Equal(cursor[8:24], hintsTargetHash(p)) {
		return fail("created for a different target")
	}
	if !bytes.Equal(cursor[24:40], hintsProgHash(p)) {
		return fail("created for a different program")
	}
	if !bytes.Equal(cursor[40:56], hintsCompsHash(comps)) {
		return fail("created for different comparisons")
	}
	rest := cursor[hintsCursorFixedSize:]
	read := func() (uint64, error) {
		v, n := binary.Uvarint(rest)
		if n <= 0 {
			return 0, fmt.Errorf("corrupted position")
		}
		rest = rest[n:]
		if v > math.MaxInt32 {
			return 0, fmt.Errorf("position value %d is out of range", v)
		}
		return v, nil
	}
	idx, err := read()
	if err != nil {
		return fail("%v", err)
	}
	if idx != uint64(callIndex) {
		return fail("created for call index %d, not %d", idx, callIndex)
	}
	var vals [3]uint64
	for i := range vals {
		vals[i], err = read()
		if err != nil {
			return fail("%v", err)
		}
	}
	if len(rest) != 0 {
		return fail("trailing %d bytes", len(rest))
	}
	pos = hintPos{arg: int(vals[0]), offset: int(vals[1]), replacer: int(vals[2])}
	return pos, nil
}
