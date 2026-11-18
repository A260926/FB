// Copyright 2026 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
	"testing"
)

// Synthetic ARM64 PE construction. Everything lives in one .text-like section
// at RVA 0x1000 / raw offset 0x200. Function bodies start at 0x1000, the
// exception directory is fixed at 0x1800 and xdata blobs are allocated from
// 0x1900 upward.
const (
	arm64TestSectionVA   = uint32(0x1000)
	arm64TestSectionRaw  = uint32(0x200)
	arm64TestPDataRVA    = uint32(0x1800)
	arm64TestXDatasStart = uint32(0x1900)
)

type arm64ImageBuilder struct {
	b       []byte
	nextRVA uint32
	pdata   [][2]uint32
	blobs   map[uint32][]byte
}

func newARM64ImageBuilder() *arm64ImageBuilder {
	im := &arm64ImageBuilder{
		b:       make([]byte, 0x1400),
		nextRVA: arm64TestXDatasStart,
		blobs:   map[uint32][]byte{},
	}
	b := im.b
	w16 := func(o int, v uint16) { binary.LittleEndian.PutUint16(b[o:], v) }
	w32 := func(o int, v uint32) { binary.LittleEndian.PutUint32(b[o:], v) }

	copy(b, "MZ")
	w32(0x3c, 0x80)
	copy(b[0x80:], []byte{'P', 'E', 0, 0})
	w16(0x84, 0xaa64) // IMAGE_FILE_MACHINE_ARM64
	w16(0x86, 1)      // one section
	w16(0x94, 240)    // SizeOfOptionalHeader
	w16(0x96, 0x2022)

	o := 0x98 // optional header (PE32+)
	w16(o, 0x20b)
	w32(o+16, 0x1000)                                    // SizeOfCode
	w32(o+20, 0x1000)                                    // SizeOfInitializedData
	binary.LittleEndian.PutUint64(b[o+24:], 0x140000000) // ImageBase
	w32(o+32, 0x1000)                                    // SectionAlignment
	w32(o+36, 0x200)                                     // FileAlignment
	w32(o+56, 0x2000)                                    // SizeOfImage
	w32(o+60, 0x200)                                     // SizeOfHeaders
	w16(o+68, 3)                                         // Subsystem console
	w32(o+108, 16)                                       // NumberOfRvaAndSizes

	s := o + 240 // section header at 0x188
	copy(b[s:], ".text")
	w32(s+8, 0x1000)               // VirtualSize
	w32(s+12, arm64TestSectionVA)  // VirtualAddress
	w32(s+16, 0x1000)              // SizeOfRawData
	w32(s+20, arm64TestSectionRaw) // PointerToRawData
	w32(s+36, 0x60000020)          // characteristics
	return im
}

func (im *arm64ImageBuilder) rvaOffset(rva uint32) uint32 {
	return rva - arm64TestSectionVA + arm64TestSectionRaw
}

// put stores a 4-byte aligned blob and returns its RVA.
func (im *arm64ImageBuilder) put(blob []byte) uint32 {
	im.nextRVA = (im.nextRVA + 3) &^ 3
	rva := im.nextRVA
	off := im.rvaOffset(rva)
	copy(im.b[off:], blob)
	im.blobs[rva] = blob
	im.nextRVA += uint32(len(blob))
	return rva
}

func (im *arm64ImageBuilder) addPData(begin, second uint32) {
	im.pdata = append(im.pdata, [2]uint32{begin, second})
}

func (im *arm64ImageBuilder) bytes() []byte {
	b := im.b
	need := im.rvaOffset(arm64TestPDataRVA) + uint32(len(im.pdata)*8)
	if uint32(len(b)) < need {
		grown := make([]byte, need)
		copy(grown, b)
		b = grown
		im.b = grown
	}
	base := im.rvaOffset(arm64TestPDataRVA)
	for i, e := range im.pdata {
		binary.LittleEndian.PutUint32(b[base+uint32(i)*8:], e[0])
		binary.LittleEndian.PutUint32(b[base+uint32(i)*8+4:], e[1])
	}
	// Data directory[3] = exception table.
	w32 := func(o int, v uint32) { binary.LittleEndian.PutUint32(b[o:], v) }
	w32(0x98+112+3*8, arm64TestPDataRVA)
	w32(0x98+116+3*8, uint32(len(im.pdata)*8))
	return b
}

func openARM64TestImage(t *testing.T, b []byte) *File {
	t.Helper()
	f, err := NewBytes(b, &Options{})
	if err != nil {
		t.Fatalf("NewBytes: %v", err)
	}
	if err := f.Parse(); err != nil {
		t.Fatalf("Parse: %v", err)
	}
	return f
}

// ---- xdata code stream helpers -------------------------------------------

type arm64Stream struct{ b []byte }

func (s *arm64Stream) code(v byte) { s.b = append(s.b, v) }

func (s *arm64Stream) wordAlign() {
	for len(s.b)%4 != 0 {
		s.b = append(s.b, 0)
	}
}

func (s *arm64Stream) operand(scaled uint32) {
	s.wordAlign()
	var w [4]byte
	binary.LittleEndian.PutUint32(w[:], scaled)
	s.b = append(s.b, w[:]...)
}

// xdata opcodes (high nibble).
const (
	xdAllocS      = 0x00 // info * 16
	xdSaveR19R20X = 0x10 // word operand (offset/8)
	xdSaveFplrX   = 0x20 // word operand
	xdAllocM      = 0x30 // (info+1)*8
	xdSaveFplr    = 0x40 // info*8
	xdSaveReg     = 0x50 // word operand
	xdSaveRegP    = 0x60
	xdSaveRegX    = 0x70
	xdSaveRegPX   = 0x80
	xdSaveLRPair  = 0x90 // word operand
	xdAllocL      = 0xa0 // word operand (size/8), two instructions
	xdSaveFreg    = 0xb0 // word operand
	xdSaveFregP   = 0xc0
	xdSaveFregX   = 0xd0
	xdSaveFregPX  = 0xe0
	xdEnd         = 0xf0
	xdEndC        = 0xf1
	xdSaveNext    = 0xf2
	xdSaveNextX   = 0xf3
	xdNop         = 0xf4
	xdSetFp       = 0xf5
	xdAddFp       = 0xf6
	xdPacSignLR   = 0xf7
	xdCustom      = 0xf8 // unsupported custom/SVE space
)

type arm64XDataSpec struct {
	FuncWords   uint32
	EBit        bool
	XBit        bool
	Extended    bool
	EpilogCount uint32
	Pool        []byte
	Handler     uint32
	Scopes      [][4]uint32 // begin instr, end instr, handler rva, jump target instr
}

func buildARM64XData(spec arm64XDataSpec) []byte {
	pool := append([]byte{}, spec.Pool...)
	if spec.XBit {
		var w [4]byte
		binary.LittleEndian.PutUint32(w[:], spec.Handler)
		pool = append(pool, w[:]...)
		binary.LittleEndian.PutUint32(w[:], uint32(len(spec.Scopes)))
		pool = append(pool, w[:]...)
		for _, sc := range spec.Scopes {
			for _, v := range sc {
				binary.LittleEndian.PutUint32(w[:], v)
				pool = append(pool, w[:]...)
			}
		}
	}
	for len(pool)%4 != 0 {
		pool = append(pool, 0)
	}
	codeWords := uint32(len(pool) / 4)

	var hdr [8]byte
	if spec.Extended {
		h := spec.FuncWords
		if spec.EBit {
			h |= 1 << 23
		}
		if spec.XBit {
			h |= 1 << 24
		}
		h |= spec.EpilogCount << 25
		// First word has the normal epilog count field zero to signal the
		// extended header.
		binary.LittleEndian.PutUint32(hdr[0:], codeWords<<24)
		binary.LittleEndian.PutUint32(hdr[4:], h)
	} else {
		h := spec.FuncWords
		if spec.EBit {
			h |= 1 << 18
		}
		if spec.XBit {
			h |= 1 << 19
		}
		h |= spec.EpilogCount << 20
		h |= codeWords << 24
		binary.LittleEndian.PutUint32(hdr[0:], h)
	}
	out := hdr[:]
	if spec.Extended {
		out = hdr[:]
	} else {
		out = hdr[0:4]
	}
	return append(out, pool...)
}

// packed word assembly.
func arm64PackedWord(flag, funcWords, regI, hcr, regF, frameCode uint32) uint32 {
	return flag |
		(funcWords&0x7ff)<<2 |
		(regI&0x3)<<13 |
		(hcr&0x1)<<15 |
		(regF&0x7)<<16 |
		(frameCode&0x1f)<<19
}

type expectedFrameOp struct {
	kind       string
	register   string
	register2  string
	offset     uint32
	writeback  uint32
	allocSize  uint32
	instrCount uint32
}

func assertOpMatches(t *testing.T, got ARM64UnwindOp, want expectedFrameOp, where string) {
	t.Helper()
	if got.Kind != want.kind || got.Register != want.register ||
		got.Register2 != want.register2 || got.Offset != want.offset ||
		got.Writeback != want.writeback || got.AllocSize != want.allocSize ||
		got.InstrCount != want.instrCount {
		t.Fatalf("%s: op mismatch\ngot  %+v\nwant kind=%s reg=%s/%s off=%#x wb=%#x alloc=%d instrs=%d",
			where, got, want.kind, want.register, want.register2,
			want.offset, want.writeback, want.allocSize, want.instrCount)
	}
}

func findFunction(t *testing.T, view *ARM64UnwindView, begin uint32) *ARM64UnwindFunction {
	t.Helper()
	for _, fn := range view.Functions() {
		if fn.BeginRVA == begin {
			return fn
		}
	}
	t.Fatalf("function beginning at %#x not found", begin)
	return nil
}

func TestARM64UnwindView(t *testing.T) {

	// Hand-written reference frame used by the packed/full equivalence and
	// forensic report cases. A 40-byte frame laid out as:
	//
	//   final SP+0  : x19, x20  (pre-indexed pair, SP -= 40)
	//   final SP+16 : d8
	//   final SP+24 : fp, lr
	//
	// Prolog (3 instructions, 12 bytes), epilog is the exact reverse.
	wantFrame := []expectedFrameOp{
		{kind: arm64OpSaveR19R20X, register: "x19", register2: "x20",
			offset: 0, writeback: 40, instrCount: 1},
		{kind: arm64OpSaveFreg, register: "d8", offset: 16, instrCount: 1},
		{kind: arm64OpSaveFplr, register: "fp", register2: "lr",
			offset: 24, instrCount: 1},
	}

	t.Run("packed_full_equivalence", func(t *testing.T) {
		im := newARM64ImageBuilder()

		packedBegin := uint32(0x1000)
		packedLen := uint32(0x40) // 16 instructions
		im.addPData(packedBegin, arm64PackedWord(1, packedLen/4, 2, 1, 1, 5))

		// Equivalent full xdata function immediately afterwards.
		fullBegin := packedBegin + packedLen
		var pro arm64Stream
		pro.code(xdSaveR19R20X)
		pro.operand(40 / 8) // save_r19r20_x, SP delta 40
		pro.code(xdSaveFreg | 0)
		pro.operand(16 / 8) // d8 at SP+16
		pro.code(xdSaveFplr | (24 / 8))
		pro.code(xdEnd)
		pro.wordAlign()

		var epi arm64Stream
		epi.code(xdSaveFplr | (24 / 8))
		epi.code(xdSaveFreg | 0)
		epi.operand(16 / 8)
		epi.code(xdSaveR19R20X)
		epi.operand(40 / 8)
		epi.code(xdEnd)
		epi.wordAlign()

		pool := append([]byte{}, pro.b...)
		// Explicit single epilog located at the function end.
		var epStart [4]byte
		binary.LittleEndian.PutUint32(epStart[:], packedLen-12)
		pool = append(pool, epStart[:]...)
		pool = append(pool, epi.b...)

		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: packedLen / 4, EpilogCount: 1, Pool: pool,
		}))
		im.addPData(fullBegin, xdataRVA) // flag = 0

		f := openARM64TestImage(t, im.bytes())
		if len(f.Exceptions) != 2 {
			t.Fatalf("existing Exceptions rows preserved: got %d, want 2", len(f.Exceptions))
		}
		view, err := f.ARM64UnwindView()
		if err != nil {
			t.Fatalf("ARM64UnwindView: %v", err)
		}
		if len(view.Functions()) != 2 {
			t.Fatalf("decoded functions: got %d, want 2", len(view.Functions()))
		}

		packedFn := findFunction(t, view, packedBegin)
		fullFn := findFunction(t, view, fullBegin)
		if packedFn.RecordKind != "packed_flag1" || fullFn.RecordKind != "full_xdata" {
			t.Fatalf("record kinds: %s / %s", packedFn.RecordKind, fullFn.RecordKind)
		}
		packedFrameOps := arm64FrameOps(packedFn.PrologOps)
		fullFrameOps := arm64FrameOps(fullFn.PrologOps)
		if len(packedFrameOps) != 3 || len(fullFrameOps) != 3 {
			t.Fatalf("prolog op counts: packed=%d full=%d",
				len(packedFrameOps), len(fullFrameOps))
		}
		for i, want := range wantFrame {
			assertOpMatches(t, packedFrameOps[i], want,
				fmt.Sprintf("packed prolog[%d]", i))
			assertOpMatches(t, fullFrameOps[i], want,
				fmt.Sprintf("full prolog[%d]", i))
			if !packedFrameOps[i].Synthetic {
				t.Fatalf("packed op %d must be marked synthetic", i)
			}
			if packedFrameOps[i].ByteSpan != 8 ||
				packedFrameOps[i].SourceRVA != arm64TestPDataRVA {
				t.Fatalf("packed op %d must carry the 8-byte pdata source", i)
			}
			if fullFrameOps[i].Synthetic {
				t.Fatalf("full xdata op %d must not be synthetic", i)
			}
		}
		if packedFn.PhysicalPrologBytes != 12 || fullFn.PhysicalPrologBytes != 12 {
			t.Fatalf("physical prolog sizes: packed=%d full=%d",
				packedFn.PhysicalPrologBytes, fullFn.PhysicalPrologBytes)
		}

		// Same relative PC must yield the same canonical pending operations.
		for _, delta := range []uint32{0, 4, 8, 12, 0x20, packedLen - 12, packedLen - 8} {
			q1, err := view.Lookup(packedBegin + delta)
			if err != nil {
				t.Fatalf("packed lookup %#x: %v", delta, err)
			}
			q2, err := view.Lookup(fullBegin + delta)
			if err != nil {
				t.Fatalf("full lookup %#x: %v", delta, err)
			}
			if q1.Phase != q2.Phase || q1.Complete != q2.Complete ||
				len(q1.PendingOps) != len(q2.PendingOps) {
				t.Fatalf("query divergence at delta %#x: %+v vs %+v",
					delta, q1, q2)
			}
			for i := range q1.PendingOps {
				a, b := q1.PendingOps[i], q2.PendingOps[i]
				if a.Kind != b.Kind || a.Register != b.Register ||
					a.Register2 != b.Register2 || a.Offset != b.Offset ||
					a.Writeback != b.Writeback || a.AllocSize != b.AllocSize {
					t.Fatalf("pending op %d divergence at delta %#x", i, delta)
				}
			}
		}
	})

	t.Run("packed_four_integer_registers", func(t *testing.T) {
		// RegI=3 saves four integer registers (x19-x22), not three: the
		// encoding jumps straight from a pair to two pairs.
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)
		// RegI=3, HCR=1 (fp/lr), RegF=0, frame code 6 => 48 bytes.
		im.addPData(begin, arm64PackedWord(1, length/4, 3, 1, 0, 6))
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}
		fn := findFunction(t, view, begin)
		want := []expectedFrameOp{
			{kind: arm64OpSaveR19R20X, register: "x19", register2: "x20",
				offset: 0, writeback: 48, instrCount: 1},
			{kind: arm64OpSaveRegP, register: "x21", register2: "x22",
				offset: 16, instrCount: 1},
			{kind: arm64OpSaveFplr, register: "fp", register2: "lr",
				offset: 32, instrCount: 1},
		}
		frame := arm64FrameOps(fn.PrologOps)
		if len(frame) != len(want) {
			t.Fatalf("four-register frame op count: got %d want %d (%+v)",
				len(frame), len(want), frame)
		}
		for i, w := range want {
			assertOpMatches(t, frame[i], w, fmt.Sprintf("regI3[%d]", i))
		}

		// A frame too small for the declared saved slots is a located error.
		bad := newARM64ImageBuilder()
		bad.addPData(begin, arm64PackedWord(1, length/4, 3, 1, 0, 1)) // only 8 bytes
		var ue *ARM64UnwindError
		if _, err := openARM64TestImage(t, bad.bytes()).ARM64UnwindView(); !errors.As(err, &ue) {
			t.Fatalf("undersized packed frame must fail with a located error, got %v", err)
		}
	})

	t.Run("partial_prolog", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)
		im.addPData(begin, arm64PackedWord(1, length/4, 2, 1, 1, 5))
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}

		// PC points at the not-yet-executed current instruction.
		q0, err := view.Lookup(begin) // nothing executed
		if err != nil {
			t.Fatal(err)
		}
		if q0.Phase != ARM64PhaseProlog || q0.Complete || len(q0.PendingOps) != 0 {
			t.Fatalf("prolog entry: %+v", q0)
		}
		if q0.Note == "" {
			t.Fatal("partial prolog must explain why the frame is incomplete")
		}

		q1, err := view.Lookup(begin + 4) // first setup done
		if err != nil {
			t.Fatal(err)
		}
		if q1.Phase != ARM64PhaseProlog || q1.Complete {
			t.Fatalf("mid prolog must not be reported as a complete frame: %+v", q1)
		}
		if len(q1.PendingOps) != 1 || q1.PendingOps[0].Kind != arm64OpSaveR19R20X {
			t.Fatalf("only the completed pair must be reversible: %+v", q1.PendingOps)
		}

		q2, err := view.Lookup(begin + 8)
		if err != nil {
			t.Fatal(err)
		}
		if len(q2.PendingOps) != 2 || q2.Complete {
			t.Fatalf("two of three setups done, frame still partial: %+v", q2)
		}
	})

	t.Run("partial_epilog", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)

		var pro arm64Stream
		pro.code(xdSaveR19R20X)
		pro.operand(5)
		pro.code(xdSaveFreg)
		pro.operand(2)
		pro.code(xdSaveFplr | 3)
		pro.code(xdEnd)
		pro.wordAlign()

		var epi arm64Stream
		epi.code(xdSaveFplr | 3)
		epi.code(xdSaveFreg)
		epi.operand(2)
		epi.code(xdSaveR19R20X)
		epi.operand(5)
		epi.code(xdEnd)
		epi.wordAlign()

		pool := append([]byte{}, pro.b...)
		var epStart [4]byte
		binary.LittleEndian.PutUint32(epStart[:], length-12)
		pool = append(pool, epStart[:]...)
		pool = append(pool, epi.b...)

		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: length / 4, EpilogCount: 1, Pool: pool,
		}))
		im.addPData(begin, xdataRVA)
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}

		epStartRVA := begin + length - 12
		qAt, err := view.Lookup(epStartRVA) // nothing restored yet
		if err != nil {
			t.Fatal(err)
		}
		if qAt.Phase != ARM64PhaseEpilog || !qAt.Complete || len(qAt.PendingOps) != 3 {
			t.Fatalf("epilog entry must still describe the full frame: %+v", qAt)
		}

		qMid, err := view.Lookup(epStartRVA + 4) // fp/lr already restored
		if err != nil {
			t.Fatal(err)
		}
		if qMid.Phase != ARM64PhaseEpilog || qMid.Complete {
			t.Fatalf("partial epilog must not look like a complete frame: %+v", qMid)
		}
		if len(qMid.PendingOps) != 2 {
			t.Fatalf("one restore already done, two must remain: %+v", qMid.PendingOps)
		}
		// Instruction-count trimming, not byte-span: the word-operand save
		// counts as one instruction even though it spans eight pool bytes.
		if qMid.PendingOps[0].Kind != arm64OpSaveFreg ||
			qMid.PendingOps[1].Kind != arm64OpSaveR19R20X {
			t.Fatalf("remaining epilog ops wrong: %+v", qMid.PendingOps)
		}

		qLast, err := view.Lookup(epStartRVA + 8)
		if err != nil {
			t.Fatal(err)
		}
		if len(qLast.PendingOps) != 1 ||
			qLast.PendingOps[0].Kind != arm64OpSaveR19R20X || qLast.Complete {
			t.Fatalf("only the final pair should remain: %+v", qLast)
		}
	})

	t.Run("end_c_phantom_fragment", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x20)

		var pro arm64Stream
		pro.code(xdAllocS | 2) // 32-byte allocation, physical prolog
		pro.code(xdEndC)
		pro.code(xdSaveReg) // phantom shrink-wrap save of x19
		pro.operand(2)      // SP+16
		pro.code(xdEnd)
		pro.wordAlign()

		var epi arm64Stream
		epi.code(xdSaveReg)
		epi.operand(2)
		epi.code(xdEnd)
		epi.wordAlign()

		pool := append([]byte{}, pro.b...)
		var epStart [4]byte
		binary.LittleEndian.PutUint32(epStart[:], length-8)
		pool = append(pool, epStart[:]...)
		pool = append(pool, epi.b...)

		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: length / 4, EpilogCount: 1, Pool: pool,
		}))
		im.addPData(begin, xdataRVA)
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}
		fn := findFunction(t, view, begin)
		if fn.PhysicalPrologBytes != 4 {
			t.Fatalf("physical prolog must stop at end_c, got %d bytes",
				fn.PhysicalPrologBytes)
		}
		var phantom *ARM64UnwindOp
		for i := range fn.PrologOps {
			if fn.PrologOps[i].Kind == arm64OpSaveReg {
				phantom = &fn.PrologOps[i]
			}
		}
		if phantom == nil || !phantom.Phantom {
			t.Fatalf("save after end_c must be marked phantom: %+v", fn.PrologOps)
		}

		// PC one instruction into the body: the phantom save is still required
		// to unwind the body, but it is not a physical instruction of this
		// fragment's prolog.
		qBody, err := view.Lookup(begin + 4)
		if err != nil {
			t.Fatal(err)
		}
		if qBody.Phase != ARM64PhaseBody || !qBody.Complete {
			t.Fatalf("body query: %+v", qBody)
		}
		var sawPhantom bool
		for _, op := range qBody.PendingOps {
			if op.Kind == arm64OpSaveReg && op.Phantom {
				sawPhantom = true
			}
		}
		if !sawPhantom {
			t.Fatal("body unwind must retain the phantom prolog operation")
		}

		qPro, err := view.Lookup(begin)
		if err != nil {
			t.Fatal(err)
		}
		if qPro.Phase != ARM64PhaseProlog || len(qPro.PendingOps) != 0 {
			t.Fatalf("phantom material must not count as physical prolog: %+v", qPro)
		}
	})

	t.Run("save_next_context", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)

		var pro arm64Stream
		pro.code(xdSaveRegP) // x19/x20 at SP+0
		pro.operand(0)
		pro.code(xdSaveNext) // x21/x22 at SP+16, context from the pair
		pro.code(xdSaveNext) // x23/x24 at SP+32
		pro.code(xdEnd)
		pro.wordAlign()

		var epi arm64Stream
		epi.code(xdSaveRegP)
		epi.operand(0)
		epi.code(xdEnd)
		epi.wordAlign()

		pool := append([]byte{}, pro.b...)
		var epStart [4]byte
		binary.LittleEndian.PutUint32(epStart[:], length-8)
		pool = append(pool, epStart[:]...)
		pool = append(pool, epi.b...)

		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: length / 4, EpilogCount: 1, Pool: pool,
		}))
		im.addPData(begin, xdataRVA)
		f := openARM64TestImage(t, im.bytes())
		view, err := f.ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}
		fn := findFunction(t, view, begin)
		want := []expectedFrameOp{
			{kind: arm64OpSaveRegP, register: "x19", register2: "x20", offset: 0, instrCount: 1},
			{kind: arm64OpSaveNext, register: "x21", register2: "x22", offset: 16, instrCount: 1},
			{kind: arm64OpSaveNext, register: "x23", register2: "x24", offset: 32, instrCount: 1},
		}
		for i, w := range want {
			assertOpMatches(t, fn.PrologOps[i], w, fmt.Sprintf("chain[%d]", i))
		}

		// Query order must never pollute the saved pair context.
		var first []ARM64UnwindOp
		for round := 0; round < 3; round++ {
			q, err := view.Lookup(begin + 0x10)
			if err != nil {
				t.Fatal(err)
			}
			if round == 0 {
				first = q.PendingOps
				continue
			}
			if len(q.PendingOps) != len(first) {
				t.Fatalf("repeated query changed the result on round %d", round)
			}
			for i := range first {
				if q.PendingOps[i].Register != first[i].Register ||
					q.PendingOps[i].Offset != first[i].Offset {
					t.Fatalf("save_next context polluted on round %d", round)
				}
			}
		}

		// A save_next with no preceding pair is a located error.
		bad := newARM64ImageBuilder()
		var badPro arm64Stream
		badPro.code(xdSaveNext)
		badPro.code(xdEnd)
		badPro.wordAlign()
		badXData := bad.put(buildARM64XData(arm64XDataSpec{
			FuncWords: 4, EpilogCount: 1,
			Pool: append(badPro.b, 0, 0, 0, 0),
		}))
		bad.addPData(0x1000, badXData)
		var ue *ARM64UnwindError
		if _, err := openARM64TestImage(t, bad.bytes()).ARM64UnwindView(); !errors.As(err, &ue) {
			t.Fatalf("save_next without pair context must return a located error, got %v", err)
		}
	})

	t.Run("shared_code_pool_and_scopes", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)

		var pro arm64Stream
		pro.code(xdSaveFplr | 3)
		pro.code(xdSaveFreg)
		pro.operand(2)
		pro.code(xdEnd)
		pro.wordAlign()

		// Two epilogs share the save_freg suffix of the code pool.
		var ep1 arm64Stream
		ep1.code(xdSaveFplr | 3)
		ep1.code(xdSaveFreg)
		ep1.operand(2)
		ep1.code(xdEnd)
		ep1.wordAlign()
		var ep2 arm64Stream
		ep2.code(xdSaveFreg)
		ep2.operand(2)
		ep2.code(xdEnd)
		ep2.wordAlign()

		pool := append([]byte{}, pro.b...)
		var st [4]byte
		binary.LittleEndian.PutUint32(st[:], length-12)
		pool = append(pool, st[:]...)
		binary.LittleEndian.PutUint32(st[:], length-8)
		pool = append(pool, st[:]...)
		pool = append(pool, ep1.b...)
		pool = append(pool, ep2.b...)

		// Scope boundaries are instruction indices; the two scopes share a
		// handler and sit fully inside the body.
		scopes := [][4]uint32{
			{4, 8, 0x1234, 0},
			{8, 12, 0x1234, 0},
		}
		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: length / 4, XBit: true, EpilogCount: 2,
			Pool: pool, Handler: 0x5678, Scopes: scopes,
		}))
		im.addPData(begin, xdataRVA)
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}
		fn := findFunction(t, view, begin)
		if len(fn.Epilogs) != 2 || len(fn.EpilogStarts) != 2 {
			t.Fatalf("epilogs: %+v", fn.Epilogs)
		}
		if fn.Epilogs[1][0].Kind != arm64OpSaveFreg ||
			fn.Epilogs[0][1].Kind != arm64OpSaveFreg {
			t.Fatal("shared suffix not decoded")
		}
		if len(fn.Scopes) != 2 {
			t.Fatalf("scopes: %+v", fn.Scopes)
		}
		if fn.Scopes[0].BeginRVA != begin+4*4 || fn.Scopes[0].EndRVA != begin+8*4 {
			t.Fatalf("scope unit conversion wrong: %+v", fn.Scopes[0])
		}
		if fn.HandlerRVA != 0x5678 || fn.HandlerDataRVA == 0 {
			t.Fatalf("handler RVA and opaque data start must both be recorded: %+v", fn)
		}
		if fn.Scopes[0].SourceRVA == 0 || fn.Scopes[0].SourceOffset == 0 {
			t.Fatal("scope record must carry its source location")
		}
	})

	t.Run("corrupt_scope_rejected", func(t *testing.T) {
		// A scope start that splits the physical prolog.
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)

		var pro arm64Stream
		pro.code(xdSaveFplr | 3)
		pro.code(xdSaveFreg)
		pro.operand(2)
		pro.code(xdEnd)
		pro.wordAlign()
		var epi arm64Stream
		epi.code(xdEnd)
		epi.wordAlign()

		pool := append([]byte{}, pro.b...)
		var st [4]byte
		binary.LittleEndian.PutUint32(st[:], length-4)
		pool = append(pool, st[:]...)
		pool = append(pool, epi.b...)

		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: length / 4, XBit: true, EpilogCount: 1, Pool: pool,
			Handler: 0x1, Scopes: [][4]uint32{{1, 6, 0x1, 0}},
		}))
		im.addPData(begin, xdataRVA)

		var ue *ARM64UnwindError
		_, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if !errors.As(err, &ue) {
			t.Fatalf("scope splitting a prolog must return a located error, got %v", err)
		}
		if ue.RVA == 0 || !strings.Contains(err.Error(), "scope") {
			t.Fatalf("error must name the scope and its location: %v", err)
		}

		// An out-of-function scope range is rejected as well.
		im2 := newARM64ImageBuilder()
		badPool := append([]byte{}, pool...)
		badX := im2.put(buildARM64XData(arm64XDataSpec{
			FuncWords: length / 4, XBit: true, EpilogCount: 1, Pool: badPool,
			Handler: 0x1, Scopes: [][4]uint32{{4, 100, 0x1, 0}},
		}))
		im2.addPData(begin, badX)
		if _, err := openARM64TestImage(t, im2.bytes()).ARM64UnwindView(); err == nil {
			t.Fatal("scope beyond the function length must be rejected")
		}
	})

	t.Run("flag2_fragment_and_flag3_unsupported", func(t *testing.T) {
		im := newARM64ImageBuilder()
		fragBegin := uint32(0x1000)
		fragLen := uint32(0x20)
		// Flag=2: no physical prolog/epilog, same canonical expansion.
		im.addPData(fragBegin, arm64PackedWord(2, fragLen/4, 1, 0, 0, 1))

		chainBegin := uint32(0x1100)
		im.addPData(chainBegin, 3|(0xabc<<2)) // flag=3 parent link

		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}
		frag := findFunction(t, view, fragBegin)
		if !frag.Fragment || frag.RecordKind != "packed_flag2" {
			t.Fatalf("flag=2 record not marked fragment: %+v", frag)
		}
		if frag.PhysicalPrologBytes != 0 || len(frag.Epilogs) != 0 {
			t.Fatal("flag=2 fragments own no physical prolog or epilog")
		}
		q, err := view.Lookup(fragBegin + 4)
		if err != nil {
			t.Fatal(err)
		}
		if q.Phase != ARM64PhaseBody || !q.Complete {
			t.Fatalf("fragment queries stay in the body: %+v", q)
		}
		// The fragment keeps the full canonical sequence (a save plus the
		// allocation) and never trims it at the fragment edges.
		if len(q.PendingOps) != 2 ||
			q.PendingOps[1].Kind != arm64OpSaveReg || q.PendingOps[1].Register != "x19" {
			t.Fatalf("fragment canonical ops must not be trimmed: %+v", q.PendingOps)
		}

		chain := findFunction(t, view, chainBegin)
		if chain.Unsupported == "" || !strings.Contains(chain.Unsupported, "flag=3") {
			t.Fatalf("flag=3 must be explicitly unsupported: %+v", chain)
		}
		// A neighboring function must still be queryable.
		if _, err := view.Lookup(fragBegin); err != nil {
			t.Fatalf("unsupported neighbor must not poison other functions: %v", err)
		}
	})

	t.Run("query_boundaries_and_bad_encodings", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x20)
		im.addPData(begin, arm64PackedWord(1, length/4, 0, 0, 0, 1))
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}

		if _, err := view.Lookup(begin + 1); err == nil ||
			!strings.Contains(err.Error(), "aligned") {
			t.Fatalf("unaligned PC must fail clearly: %v", err)
		}
		if _, err := view.Lookup(begin + length); err == nil ||
			!strings.Contains(err.Error(), "outside every function") {
			t.Fatalf("half-open end RVA must be outside the function: %v", err)
		}
		if _, err := view.Lookup(0x4000); err == nil {
			t.Fatal("unmapped PC must fail")
		}

		// Unknown / custom opcode must never be silently treated as nop.
		bad1 := newARM64ImageBuilder()
		var s1 arm64Stream
		s1.code(xdCustom)
		s1.code(xdEnd)
		s1.wordAlign()
		r1 := bad1.put(buildARM64XData(arm64XDataSpec{
			FuncWords: 4, EpilogCount: 1,
			Pool: append(s1.b, 0, 0, 0, 0),
		}))
		bad1.addPData(0x1000, r1)
		if _, err := openARM64TestImage(t, bad1.bytes()).ARM64UnwindView(); err == nil ||
			!strings.Contains(err.Error(), "unknown") {
			t.Fatalf("custom opcode must be reported unknown: %v", err)
		}

		// Unterminated sequence.
		bad2 := newARM64ImageBuilder()
		var s2 arm64Stream
		s2.code(xdNop) // no end, code words exhausted afterwards
		r2 := bad2.put(buildARM64XData(arm64XDataSpec{
			FuncWords: 4, EpilogCount: 1, Pool: s2.b,
		}))
		bad2.addPData(0x1000, r2)
		if _, err := openARM64TestImage(t, bad2.bytes()).ARM64UnwindView(); err == nil ||
			!strings.Contains(err.Error(), "terminated") {
			t.Fatalf("unterminated sequence must be rejected: %v", err)
		}

		// Illegal extended header (version bit set in the second word).
		bad3 := newARM64ImageBuilder()
		ext := make([]byte, 8)
		binary.LittleEndian.PutUint32(ext[0:], 1<<24) // code words, zero epilog count
		binary.LittleEndian.PutUint32(ext[4:], 1<<31) // illegal version bit
		r3 := bad3.put(ext)
		bad3.addPData(0x1000, r3)
		if _, err := openARM64TestImage(t, bad3.bytes()).ARM64UnwindView(); err == nil ||
			!strings.Contains(err.Error(), "extended xdata header") {
			t.Fatalf("illegal extended header must be rejected: %v", err)
		}

		// Overlapping functions.
		bad4 := newARM64ImageBuilder()
		bad4.addPData(0x1000, arm64PackedWord(1, 8, 0, 0, 0, 0))
		bad4.addPData(0x1010, arm64PackedWord(1, 8, 0, 0, 0, 0))
		if _, err := openARM64TestImage(t, bad4.bytes()).ARM64UnwindView(); err == nil ||
			!strings.Contains(err.Error(), "overlap") {
			t.Fatalf("overlapping functions must be rejected: %v", err)
		}
	})

	t.Run("full_opcode_coverage", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		funcBytes := uint32(0x40) // 16 instructions

		var pro arm64Stream
		pro.code(xdPacSignLR)  // 1 instr
		pro.code(xdAllocM)     // (0+1)*8 = 8 bytes, 1 instr
		pro.code(xdSaveLRPair) // lr/x19 pair, word operand
		pro.operand(2)         // SP+16
		pro.code(xdSetFp)      // 1 instr
		pro.code(xdAddFp)      // 1 instr
		pro.code(xdSaveRegX)   // x19 pre-indexed, word operand
		pro.operand(4)         // SP delta 32
		pro.code(xdSaveFregP)  // d8/d9 pair, word operand
		pro.operand(1)         // SP+8
		pro.code(xdNop)        // 1 instr
		pro.code(xdAllocL)     // two-instruction allocation
		pro.operand(64)        // 512 bytes
		pro.code(xdEnd)
		pro.wordAlign()

		var epi arm64Stream
		epi.code(xdNop)
		epi.code(xdEnd)
		epi.wordAlign()

		pool := append([]byte{}, pro.b...)
		var st [4]byte
		binary.LittleEndian.PutUint32(st[:], funcBytes-4)
		pool = append(pool, st[:]...)
		pool = append(pool, epi.b...)

		xdataRVA := im.put(buildARM64XData(arm64XDataSpec{
			FuncWords: funcBytes / 4, EpilogCount: 1, Pool: pool,
		}))
		im.addPData(begin, xdataRVA)
		view, err := openARM64TestImage(t, im.bytes()).ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}
		fn := findFunction(t, view, begin)

		want := []expectedFrameOp{
			{kind: arm64OpPacSignLR, instrCount: 1},
			{kind: arm64OpAllocM, allocSize: 8, instrCount: 1},
			{kind: arm64OpSaveLRPair, register: "lr", register2: "x19",
				offset: 16, instrCount: 1},
			{kind: arm64OpSetFp, instrCount: 1},
			{kind: arm64OpAddFp, instrCount: 1},
			{kind: arm64OpSaveRegX, register: "x19", offset: 0,
				writeback: 32, instrCount: 1},
			{kind: arm64OpSaveFregP, register: "d8", register2: "d9",
				offset: 8, instrCount: 1},
			{kind: arm64OpNop, instrCount: 1},
			{kind: arm64OpAllocL, allocSize: 512, instrCount: 2},
		}
		frame := arm64FrameOps(fn.PrologOps)
		if len(frame) != len(want) {
			t.Fatalf("frame op count: got %d want %d", len(frame), len(want))
		}
		for i, w := range want {
			assertOpMatches(t, frame[i], w, fmt.Sprintf("opcode coverage[%d]", i))
		}
		// alloc_l represents two instructions; its raw span covers the code
		// byte, the alignment padding and the 32-bit operand word.
		if frame[8].ByteSpan != 7 || frame[8].InstrCount != 2 {
			t.Fatalf("alloc_l span/instrs wrong: %+v", frame[8])
		}
		// 10 represented instructions => 40 physical prolog bytes.
		if fn.PhysicalPrologBytes != 40 {
			t.Fatalf("physical prolog bytes: got %d want 40", fn.PhysicalPrologBytes)
		}

		// Two instructions in: only pac_sign_lr and alloc_m are reversible.
		qEarly, err := view.Lookup(begin + 8)
		if err != nil {
			t.Fatal(err)
		}
		if qEarly.Phase != ARM64PhaseProlog || qEarly.Complete ||
			len(qEarly.PendingOps) != 2 {
			t.Fatalf("early prolog query wrong: %+v", qEarly)
		}
		if qEarly.PendingOps[0].Kind != arm64OpAllocM ||
			qEarly.PendingOps[1].Kind != arm64OpPacSignLR {
			t.Fatalf("pending order must be inverse execution: %+v", qEarly.PendingOps)
		}

		// Body keeps every frame operation, including nop and the 2-instr
		// alloc_l, while the terminator is dropped.
		qBody, err := view.Lookup(begin + 0x28)
		if err != nil {
			t.Fatal(err)
		}
		if qBody.Phase != ARM64PhaseBody || !qBody.Complete ||
			len(qBody.PendingOps) != len(want) {
			t.Fatalf("body query wrong: %+v", qBody)
		}

		// Epilog entry: its own single restore is a complete pending set.
		qEp, err := view.Lookup(begin + funcBytes - 4)
		if err != nil {
			t.Fatal(err)
		}
		if qEp.Phase != ARM64PhaseEpilog || !qEp.Complete ||
			len(qEp.PendingOps) != 1 || qEp.PendingOps[0].Kind != arm64OpNop {
			t.Fatalf("epilog query wrong: %+v", qEp)
		}
	})

	t.Run("forensic_report_evidence", func(t *testing.T) {
		im := newARM64ImageBuilder()
		begin := uint32(0x1000)
		length := uint32(0x40)
		im.addPData(begin, arm64PackedWord(1, length/4, 2, 1, 1, 5))
		f := openARM64TestImage(t, im.bytes())
		view, err := f.ARM64UnwindView()
		if err != nil {
			t.Fatalf("view: %v", err)
		}

		// Independently hand-written expected frame layout.
		handWritten := []expectedFrameOp{
			{kind: arm64OpSaveR19R20X, register: "x19", register2: "x20",
				offset: 0, writeback: 40, instrCount: 1},
			{kind: arm64OpSaveFreg, register: "d8", offset: 16, instrCount: 1},
			{kind: arm64OpSaveFplr, register: "fp", register2: "lr",
				offset: 24, instrCount: 1},
		}
		fn := findFunction(t, view, begin)
		if len(fn.PrologOps) != len(handWritten) {
			t.Fatalf("frame layout mismatch")
		}
		for i, w := range handWritten {
			assertOpMatches(t, fn.PrologOps[i], w, fmt.Sprintf("handwritten[%d]", i))
		}

		var buf bytes.Buffer
		pcs := []uint32{
			begin + 8,          // prolog middle: partial evidence
			begin + 0x20,       // body: complete frame
			begin + length - 8, // epilog middle: partial evidence
			begin + 1,          // invalid alignment
		}
		if err := view.WriteReport(&buf, pcs); err != nil {
			t.Fatalf("report: %v", err)
		}
		out := buf.String()

		mustContain := []string{
			fmt.Sprintf("function [%#06x,%#06x)", begin, begin+length),
			"phase=prolog complete_frame=false",
			"EVIDENCE PARTIAL",
			"phase=body complete_frame=true",
			"phase=epilog complete_frame=false",
			fmt.Sprintf("pdata=rva %#x", arm64TestPDataRVA),
			"packed-pdata(synthetic)",
			"EVIDENCE UNTRUSTED", // the unaligned PC
			"save_r19r20_x",
			"save_fplr",
		}
		for _, want := range mustContain {
			if !strings.Contains(out, want) {
				t.Fatalf("report missing %q\nfull report:\n%s", want, out)
			}
		}
		// A partial frame must never be printed as a complete one, and an
		// empty operation list must never masquerade as a full frame.
		if strings.Contains(out, "phase=prolog complete_frame=true") {
			t.Fatal("partial prolog falsely reported complete")
		}

		// A corrupted image contributes no view at all: the caller records
		// why the evidence is untrusted instead of inventing an empty frame.
		corrupt := newARM64ImageBuilder()
		var pro arm64Stream
		pro.code(xdNop) // unterminated
		corruptX := corrupt.put(buildARM64XData(arm64XDataSpec{
			FuncWords: 4, EpilogCount: 1, Pool: pro.b,
		}))
		corrupt.addPData(0x1000, corruptX)
		var cbuf bytes.Buffer
		cview, cerr := openARM64TestImage(t, corrupt.bytes()).ARM64UnwindView()
		if cerr == nil {
			t.Fatal("corrupt image unexpectedly produced a view")
		}
		fmt.Fprintf(&cbuf, "PC rva=%#06x\n  EVIDENCE UNTRUSTED: %v\n", 0x1000, cerr)
		if !strings.Contains(cbuf.String(), "EVIDENCE UNTRUSTED") {
			t.Fatal("corrupt evidence must be flagged")
		}
		if cview != nil {
			t.Fatal("no partial view may be published for a corrupt image")
		}
	})
}

// ExampleARM64UnwindView demonstrates a read-only offline query against the
// static unwind metadata of a fixed ARM64 image.
func ExampleARM64UnwindView() {
	im := newARM64ImageBuilder()
	begin := uint32(0x1000)
	im.addPData(begin, arm64PackedWord(1, 16, 2, 1, 1, 5))
	f, err := NewBytes(im.bytes(), &Options{})
	if err != nil {
		panic(err)
	}
	if err := f.Parse(); err != nil {
		panic(err)
	}
	view, err := f.ARM64UnwindView()
	if err != nil {
		panic(err)
	}
	q, err := view.Lookup(begin + 8) // two of three prolog instructions done
	if err != nil {
		panic(err)
	}
	fmt.Printf("phase=%s complete=%v pending=%d first=%s\n",
		q.Phase, q.Complete, len(q.PendingOps), q.PendingOps[0].Kind)
	// Output: phase=prolog complete=false pending=2 first=save_freg
}
