package macho

// Contract tests for the offline compact unwind index. Every fixture is a
// legal, read-only little-endian Mach-O64/AMD64 image built in this file from
// types.Segment64/Section64 and raw bytes — no external binaries, no cgo.
// Expected values are literals computed by hand below; the methods under test
// are never used to generate them.

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"

	"github.com/blacktop/go-macho/task_support/taskcheck"
	"github.com/blacktop/go-macho/types"
)

const cuTestBase uint64 = 0x100000000

// Fixture layout (image-relative offsets equal file offsets):
//
//	0x1000-0x2fff __text          (function ranges, IND immediates, personality slots)
//	0x4000-0x7fff __unwind_info   (header 0, common 0x40, personalities 0x80,
//	                              first level 0xc0, LSDA tables 0x100..,
//	                              pages 0x1000 and 0x2000 inside the section)
//	0x8000-0x80ff __eh_frame      (when requested)
const (
	cuTextOff      = 0x1000
	cuTextSize     = 0x2000
	cuUnwindOff    = 0x4000
	cuUnwindSize   = 0x4000
	cuEHOff        = 0x8000
	cuEHSize       = 0x100
	cuCommonSecOff = 0x40
	cuPersSecOff   = 0x80
	cuIndexSecOff  = 0xc0
	cuLSDABaseOff  = 0x100
)

type cuLSDASpec struct {
	funcOff uint32
	lsdaOff uint32
}

type cuRegularEntry struct {
	off          uint32
	continuation bool
	encoding     uint32
}

type cuCompressedEntry struct {
	delta        uint32
	encIndex     uint32
	continuation bool
}

type cuPageSpec struct {
	start      uint32 // first-level function offset for this page
	kind       uint32 // 2 regular, 3 compressed
	regular    []cuRegularEntry
	compressed []cuCompressedEntry
	locals     []uint32
}

type cuLevelSpec struct {
	page cuPageSpec
	lsda []cuLSDASpec
}

type cuImmediate struct {
	imageOff uint32
	value    uint32
}

type cuImageSpec struct {
	levels        []cuLevelSpec
	sentinelEnd   uint32
	common        []uint32
	personalities []int32
	slotImageOff  uint32 // file/VM offset of the 8-byte personality slot region
	slotRaw       []byte // len == 8*len(personalities)
	immediates    []cuImmediate
	ehFrame       bool
}

// buildCUImage serializes a standalone MH_EXECUTE Mach-O64 containing the
// described unwind_info. Second-level pages are stored in reverse first-level
// order physically, exercising the rule that pages need not be ordered.
func buildCUImage(spec cuImageSpec) ([]byte, error) {
	fileSize := uint64(cuEHOff + cuEHSize)
	raw := make([]byte, fileSize)

	var segName, textName, unwindName, ehName [16]byte
	copy(segName[:], "__TEXT")
	copy(textName[:], "__text")
	copy(unwindName[:], "__unwind_info")
	copy(ehName[:], "__eh_frame")

	nsect := uint32(2)
	if spec.ehFrame {
		nsect = 3
	}
	cmdSize := uint32(binary.Size(types.Segment64{}) + int(nsect)*binary.Size(types.Section64{}))
	header := types.FileHeader{
		Magic: types.Magic64, CPU: types.CPUAmd64, Type: types.MH_EXECUTE,
		NCommands: 1, SizeCommands: cmdSize,
	}
	hdr := make([]byte, types.FileHeaderSize64)
	header.Put(hdr, binary.LittleEndian)
	copy(raw[0:], hdr)

	seg := types.Segment64{
		LoadCmd: types.LC_SEGMENT_64, Len: cmdSize, Name: segName,
		Addr: cuTestBase, Memsz: fileSize, Filesz: fileSize, Maxprot: 7, Prot: 5, Nsect: nsect,
	}
	var buf bytes.Buffer
	if err := writeBinary(&buf, seg); err != nil {
		return nil, err
	}
	sections := []types.Section64{
		{Name: textName, Seg: segName, Addr: cuTestBase + cuTextOff, Size: cuTextSize, Offset: cuTextOff, Flags: types.PURE_INSTRUCTIONS},
		{Name: unwindName, Seg: segName, Addr: cuTestBase + cuUnwindOff, Size: cuUnwindSize, Offset: cuUnwindOff},
	}
	if spec.ehFrame {
		sections = append(sections, types.Section64{Name: ehName, Seg: segName, Addr: cuTestBase + cuEHOff, Size: cuEHSize, Offset: cuEHOff})
	}
	for _, s := range sections {
		if err := writeBinary(&buf, s); err != nil {
			return nil, err
		}
	}
	if buf.Len() > cuTextOff {
		return nil, errors.New("load commands overlap __text")
	}
	copy(raw[types.FileHeaderSize64:], buf.Bytes())

	for _, imm := range spec.immediates {
		binary.LittleEndian.PutUint32(raw[imm.imageOff:], imm.value)
	}
	if spec.slotImageOff != 0 {
		copy(raw[spec.slotImageOff:], spec.slotRaw)
	}

	u := raw[cuUnwindOff : cuUnwindOff+cuUnwindSize]
	put32 := func(off uint32, v uint32) { binary.LittleEndian.PutUint32(u[off:], v) }

	put32(0, 1)
	if len(spec.common) > 0 {
		put32(4, cuCommonSecOff)
		put32(8, uint32(len(spec.common)))
		for i, enc := range spec.common {
			put32(cuCommonSecOff+uint32(i)*4, enc)
		}
	}
	if len(spec.personalities) > 0 {
		put32(12, cuPersSecOff)
		put32(16, uint32(len(spec.personalities)))
		for i, delta := range spec.personalities {
			put32(cuPersSecOff+uint32(i)*4, uint32(delta))
		}
	}
	put32(20, cuIndexSecOff)
	put32(24, uint32(len(spec.levels)+1))

	// LSDA sub-tables, packed in first-level order.
	lsdaStarts := make([]uint32, len(spec.levels))
	lsdaCursor := uint32(cuLSDABaseOff)
	for i, level := range spec.levels {
		lsdaStarts[i] = lsdaCursor
		for _, rec := range level.lsda {
			put32(lsdaCursor, rec.funcOff)
			put32(lsdaCursor+4, rec.lsdaOff)
			lsdaCursor += 8
		}
	}

	// First-level index, including the sentinel tail.
	for i, level := range spec.levels {
		item := cuIndexSecOff + uint32(i)*12
		pageSecOff := uint32(0x1000 + (len(spec.levels)-1-i)*0x1000)
		put32(item, level.page.start)
		put32(item+4, pageSecOff)
		put32(item+8, lsdaStarts[i])
	}
	sentinel := cuIndexSecOff + uint32(len(spec.levels))*12
	put32(sentinel, spec.sentinelEnd)
	put32(sentinel+4, 0)
	put32(sentinel+8, lsdaCursor)

	// Pages, in reverse physical order relative to the levels.
	for i, level := range spec.levels {
		pageOff := uint32(0x1000 + (len(spec.levels)-1-i)*0x1000)
		page := level.page
		put32(pageOff, page.kind)
		switch page.kind {
		case 2:
			binary.LittleEndian.PutUint16(u[pageOff+4:], 8)
			binary.LittleEndian.PutUint16(u[pageOff+6:], uint16(len(page.regular)))
			for j, e := range page.regular {
				at := pageOff + 8 + uint32(j)*8
				word := e.off
				if e.continuation {
					word |= 0x80000000
				}
				put32(at, word)
				put32(at+4, e.encoding)
			}
		case 3:
			n := uint32(len(page.compressed))
			encPageOff := uint32(12) + n*4
			binary.LittleEndian.PutUint16(u[pageOff+4:], 12)
			binary.LittleEndian.PutUint16(u[pageOff+6:], uint16(n))
			binary.LittleEndian.PutUint16(u[pageOff+8:], uint16(encPageOff))
			binary.LittleEndian.PutUint16(u[pageOff+10:], uint16(len(page.locals)))
			for j, e := range page.compressed {
				word := e.delta & 0xffffff
				word |= e.encIndex << 24
				if e.continuation {
					word |= 0x80000000
				}
				put32(pageOff+12+uint32(j)*4, word)
			}
			for j, enc := range page.locals {
				put32(pageOff+encPageOff+uint32(j)*4, enc)
			}
		default:
			return nil, errors.New("bad page kind in spec")
		}
	}
	return raw, nil
}

func writeBinary(buf *bytes.Buffer, v any) error {
	return binary.Write(buf, binary.LittleEndian, v)
}

func newCUFile(t *testing.T, raw []byte) *File {
	t.Helper()
	f, err := NewFile(bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	return f
}

func buildIndex(t *testing.T, raw []byte) *CompactUnwindIndex {
	t.Helper()
	f := newCUFile(t, raw)
	t.Cleanup(func() { _ = f.Close() })
	idx, err := f.BuildCompactUnwindIndex()
	if err != nil {
		t.Fatalf("BuildCompactUnwindIndex: %v", err)
	}
	return idx
}

// permEncode is the test-side inverse of the production permutation decoder.
// It exists only to build fixtures; assertions also pin literal code values.
func permEncode(regs []CompactRegister) uint32 {
	universe := []CompactRegister{CompactRegRBX, CompactRegR12, CompactRegR13, CompactRegR14, CompactRegR15, CompactRegRBP}
	remaining := append([]CompactRegister(nil), universe...)
	var code uint32
	for i, reg := range regs {
		pick := uint32(0)
		for j, cand := range remaining {
			if cand == reg {
				pick = uint32(j)
				break
			}
		}
		code = code*uint32(6-i) + pick
		remaining = append(remaining[:pick], remaining[pick+1:]...)
	}
	return code
}

// releasableReaderAt lets a test prove the index owns its bytes.
type releasableReaderAt struct {
	b        []byte
	released bool
}

func (r *releasableReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if r.released {
		return 0, errors.New("fixture bytes released")
	}
	if off < 0 || off >= int64(len(r.b)) {
		return 0, io.EOF
	}
	n := copy(p, r.b[off:])
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

func TestCompactPages(t *testing.T) {
	t.Parallel()
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			encA     = uint32(0x02020000) // STACK_IMMD, 16 bytes, no saved regs
			encB     = uint32(0x02030000) // STACK_IMMD, 24 bytes
			encLocal = uint32(0x02040000) // STACK_IMMD, 32 bytes
		)
		spec := cuImageSpec{
			common: []uint32{encA, encB},
			levels: []cuLevelSpec{
				{
					page: cuPageSpec{
						start: 0x1000,
						kind:  2,
						regular: []cuRegularEntry{
							{off: 0x1000, encoding: 0},          // zero encoding retained as no_unwind
							{off: 0x1100, encoding: 0x02050000}, // 40 bytes, inline
							{off: 0x1200, continuation: true, encoding: 0x02060000},
						},
					},
				},
				{
					page: cuPageSpec{
						start: 0x2000,
						kind:  3,
						compressed: []cuCompressedEntry{
							{delta: 0x00, encIndex: 0}, // common[0]
							{delta: 0x50, encIndex: 2}, // local[0]
							{delta: 0xa0, encIndex: 1, continuation: true},
							{delta: 0xf0, encIndex: 0},
						},
						locals: []uint32{encLocal},
					},
				},
			},
			sentinelEnd: 0x2200,
		}
		raw, err := buildCUImage(spec)
		c.Check(err == nil, "build image: %v", err)
		idx := buildIndex(t, raw)

		// Pages are stored in reverse physical order: level 0's regular page
		// sits at section offset 0x2000, level 1's compressed page at 0x1000.
		entries := idx.Entries()
		c.Check(len(entries) == 7, "entries = %d, want 7 (sentinel excluded)", len(entries))

		look := func(off uint32) *CompactUnwindEntry {
			e, lerr := idx.Lookup(cuTestBase + uint64(off))
			c.Check(lerr == nil, "Lookup(%#x): %v", off, lerr)
			return e
		}
		miss := func(off uint32) {
			_, lerr := idx.Lookup(cuTestBase + uint64(off))
			c.Check(errors.Is(lerr, ErrCompactUnwindNotFound), "Lookup(%#x) error = %v, want not found", off, lerr)
		}

		// Zero-encoded first range survives as its own no_unwind range.
		e := look(0x1000)
		c.Check(e.NoUnwind && e.Rule == nil, "first entry should be no_unwind")
		c.Check(e.RangeStart == cuTestBase+0x1000 && e.RangeEnd == cuTestBase+0x1100, "zero range half-open bounds")
		c.Check(e.Page.Kind == 2 && e.Page.SectionOffset == 0x2000, "regular page provenance (reverse storage)")
		c.Check(e.EncodingSource.Kind == CompactEncodingInline, "regular page encoding is inline")
		c.Check(len(e.Entry.Raw) == 8, "regular entry keeps 8 raw bytes")
		c.Check(binary.LittleEndian.Uint32(e.Entry.Raw) == 0x1000, "raw entry function offset")

		// Half-open boundary: the last byte before 0x1100 still belongs to f0.
		e = look(0x10ff)
		c.Check(e.RangeStart == cuTestBase+0x1000, "zero range not swallowed by neighbor at %#x", 0x10ff)

		e = look(0x1100)
		c.Check(e.RangeEnd == cuTestBase+0x1200, "second range bounds")
		c.Check(e.RawEncoding == 0x02050000, "inline encoding preserved")

		// Continuation keeps its own range start but the logical function start.
		e = look(0x1200)
		c.Check(e.IsContinuation, "0x1200 must be a continuation")
		c.Check(e.RangeStart == cuTestBase+0x1200, "continuation RangeStart")
		c.Check(e.FunctionStart == cuTestBase+0x1100, "continuation FunctionStart anchors to same-page parent")
		// The page's last range is closed by the next first-level item, not by
		// any physical page position.
		c.Check(e.RangeEnd == cuTestBase+0x2000, "last page range closed by next first-level item")
		e = look(0x1fff)
		c.Check(e.IsContinuation && e.RangeStart == cuTestBase+0x1200, "continuation covers up to level boundary")

		// Compressed page: deltas are relative to the first-level start.
		e = look(0x2000)
		c.Check(e.RangeStart == cuTestBase+0x2000 && e.RangeEnd == cuTestBase+0x2050, "compressed first range")
		c.Check(e.EncodingSource.Kind == CompactEncodingCommon && e.EncodingSource.Index == 0, "common encoding selected first")
		c.Check(e.RawEncoding == encA, "common[0] resolved")
		c.Check(e.Page.Kind == 3 && e.Page.SectionOffset == 0x1000, "compressed page provenance")

		e = look(0x2050)
		c.Check(e.EncodingSource.Kind == CompactEncodingLocal && e.EncodingSource.Index == 0, "local encoding selected after common")
		c.Check(e.RawEncoding == encLocal, "local[0] resolved")

		e = look(0x20a0)
		c.Check(e.IsContinuation && e.FunctionStart == cuTestBase+0x2050, "compressed continuation anchor")
		c.Check(e.EncodingSource.Kind == CompactEncodingCommon && e.EncodingSource.Index == 1, "continuation common[1]")
		c.Check(e.RawEncoding == encB, "common[1] resolved through continuation")

		e = look(0x20f0)
		c.Check(e.RangeEnd == cuTestBase+0x2200, "final range ends at sentinel function offset")
		e = look(0x21ff)
		c.Check(e.RangeStart == cuTestBase+0x20f0, "final range covers its last byte")

		miss(0x2200) // sentinel is not a function and covers nothing
		miss(0x0fff) // below the first level
		_, lerr := idx.Lookup(cuTestBase - 1)
		c.Check(errors.Is(lerr, ErrCompactUnwindNotFound), "below-base lookup: %v", lerr)
		_, lerr = idx.Lookup(0)
		c.Check(errors.Is(lerr, ErrCompactUnwindNotFound), "zero address lookup: %v", lerr)

		// Sentinel-only image: legal empty table, empty queryable index.
		emptyRaw, err := buildCUImage(cuImageSpec{sentinelEnd: 0x1000})
		c.Check(err == nil, "build empty image: %v", err)
		emptyIdx := buildIndex(t, emptyRaw)
		c.Check(len(emptyIdx.Entries()) == 0, "sentinel-only index has no entries")
		_, lerr = emptyIdx.Lookup(cuTestBase + 0x1000)
		c.Check(errors.Is(lerr, ErrCompactUnwindNotFound), "empty index lookup: %v", lerr)
	})
}

func TestCompactRecipes(t *testing.T) {
	t.Parallel()
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Hand-picked frameless permutation: R15 then R13 then RBP.
		// picks are 4 (radix 6), 2 (radix 5), 3 (radix 4): 4*5*4+2*4+3 = 91.
		perm := permEncode([]CompactRegister{CompactRegR15, CompactRegR13, CompactRegRBP})
		c.Check(perm == 0x5b, "permutation code = %#x, want 0x5b", perm)

		const (
			rbpWord     = uint32(0x01020421) // mode 1, offset 2, slots RBX, R14, _, R12
			immdWord    = uint32(0x02060c5b) // mode 2, 48 bytes, 3 regs, perm 0x5b
			indWord     = uint32(0x03102400) // mode 3, 1 reg RBX, rel 0x10, adjust +8
			dwarfWord   = uint32(0x04000020)
			zeroWord    = uint32(0)
			unknownWord = uint32(0x07000000) // mode 7 is not defined
		)
		spec := cuImageSpec{
			common: []uint32{rbpWord, immdWord, indWord, dwarfWord, zeroWord, unknownWord},
			levels: []cuLevelSpec{{
				page: cuPageSpec{
					start: 0x1000,
					kind:  3,
					compressed: []cuCompressedEntry{
						{delta: 0x000, encIndex: 0}, // RBP_FRAME
						{delta: 0x100, encIndex: 1}, // STACK_IMMD
						{delta: 0x400, encIndex: 2}, // STACK_IND
						{delta: 0x500, encIndex: 3}, // DWARF
						{delta: 0x600, encIndex: 4}, // no_unwind
						{delta: 0x700, encIndex: 5}, // unknown mode
					},
				},
			}},
			sentinelEnd: 0x2000,
			immediates: []cuImmediate{
				{imageOff: 0x1410, value: 0x40}, // IND function 0x1400 + rel 0x10
			},
			ehFrame: true,
		}
		raw, err := buildCUImage(spec)
		c.Check(err == nil, "build recipe image: %v", err)
		idx := buildIndex(t, raw)

		// RBP_FRAME: CFA = RBP+16, RIP at CFA-8; slot i is RBP-8*(offset+i).
		e, lerr := idx.Lookup(cuTestBase + 0x1000)
		c.Check(lerr == nil, "lookup RBP: %v", lerr)
		c.Check(e.Rule != nil && e.Rule.Mode == CompactModeRBPFrame, "RBP mode")
		c.Check(e.Rule.CFA.Base == CompactRegRBP && e.Rule.CFA.Offset == 16, "RBP CFA expression")
		c.Check(e.Rule.RIP.OffsetFromCFA == -8, "RIP slot")
		wantRBP := []CompactSavedRegister{
			{Register: CompactRegRBX, Slot: CompactSlot{OffsetFromCFA: -32}},
			{Register: CompactRegR14, Slot: CompactSlot{OffsetFromCFA: -40}},
			{Register: CompactRegR12, Slot: CompactSlot{OffsetFromCFA: -56}},
		}
		c.Check(savedEqual(e.Rule.SavedRegisters, wantRBP), "RBP saved slots = %+v", e.Rule.SavedRegisters)

		// STACK_IMMD: CFA = RSP+48, permutation decoded as an ordered list.
		e, lerr = idx.Lookup(cuTestBase + 0x1100)
		c.Check(lerr == nil, "lookup IMMD: %v", lerr)
		c.Check(e.Rule.Mode == CompactModeStackIMMD && e.Rule.StackSize == 48, "IMMD stack size")
		c.Check(e.Rule.CFA.Base == CompactRegRSP && e.Rule.CFA.Offset == 48, "IMMD CFA")
		wantIMMD := []CompactSavedRegister{
			{Register: CompactRegR15, Slot: CompactSlot{OffsetFromCFA: -32}},
			{Register: CompactRegR13, Slot: CompactSlot{OffsetFromCFA: -24}},
			{Register: CompactRegRBP, Slot: CompactSlot{OffsetFromCFA: -16}},
		}
		c.Check(savedEqual(e.Rule.SavedRegisters, wantIMMD), "IMMD ordered permutation = %+v", e.Rule.SavedRegisters)

		// STACK_IND: indirect size read through the File mapping, provenance kept.
		e, lerr = idx.Lookup(cuTestBase + 0x1400)
		c.Check(lerr == nil, "lookup IND: %v", lerr)
		c.Check(e.Rule.Mode == CompactModeStackIND, "IND mode")
		c.Check(e.Rule.Immediate != nil, "IND keeps immediate")
		imm := e.Rule.Immediate
		c.Check(imm.VMAddress == cuTestBase+0x1410 && imm.FileOffset == 0x1410, "IND immediate location")
		c.Check(imm.Value == 0x40 && imm.Adjustment == 8 && imm.StackSize == 72, "IND indirect stack size")
		c.Check(bytes.Equal(imm.Raw[:], []byte{0x40, 0, 0, 0}), "IND raw immediate bytes")
		c.Check(e.Rule.StackSize == 72 && e.Rule.CFA.Offset == 72, "IND CFA uses indirect size")
		wantIND := []CompactSavedRegister{{Register: CompactRegRBX, Slot: CompactSlot{OffsetFromCFA: -16}}}
		c.Check(savedEqual(e.Rule.SavedRegisters, wantIND), "IND saved slot = %+v", e.Rule.SavedRegisters)

		// DWARF fallback: reference location only, no compact rule or CFI.
		e, lerr = idx.Lookup(cuTestBase + 0x1500)
		c.Check(lerr == nil, "lookup DWARF: %v", lerr)
		c.Check(e.Rule == nil && e.DWARF != nil, "DWARF produces no compact rule")
		c.Check(e.DWARF.EHFrameSectionOffset == 0x20, "FDE section offset")
		c.Check(e.DWARF.EHFrameVMAddress == cuTestBase+cuEHOff+0x20, "FDE VM address")
		c.Check(e.DWARF.EHFrameFileOffset == cuEHOff+0x20, "FDE file offset")
		c.Check(e.DWARF.Source.SectionOffset != 0, "DWARF reference keeps unwind_info source")

		// no_unwind and unknown mode are distinct states, neither fabricates a rule.
		e, lerr = idx.Lookup(cuTestBase + 0x1600)
		c.Check(lerr == nil, "lookup zero: %v", lerr)
		c.Check(e.NoUnwind && !e.UnknownMode && e.Rule == nil, "zero encoding is no_unwind")
		e, lerr = idx.Lookup(cuTestBase + 0x1700)
		c.Check(lerr == nil, "lookup unknown: %v", lerr)
		c.Check(e.UnknownMode && !e.NoUnwind && e.Rule == nil, "mode 7 is unsupported/unknown, raw word %#x", e.RawEncoding)

		// Boundary addresses inside the same image.
		e, lerr = idx.Lookup(cuTestBase + 0x10ff)
		c.Check(lerr == nil && e.RangeStart == cuTestBase+0x1000, "entry interior lookup")
		_, lerr = idx.Lookup(cuTestBase + 0x800)
		c.Check(errors.Is(lerr, ErrCompactUnwindNotFound), "gap address not found: %v", lerr)

		// Malformed inputs: a half-built index must never be returned.
		// 1. Truncated backing bytes.
		limited := io.NewSectionReader(bytes.NewReader(raw), 0, int64(len(raw))-0x2000)
		f, ferr := NewFile(limited)
		c.Check(ferr == nil, "NewFile limited: %v", ferr)
		_, berr := f.BuildCompactUnwindIndex()
		c.Check(berr != nil && containsText(berr.Error(), "section offset"), "truncated section error carries offset: %v", berr)
		_ = f.Close()

		// 2. Compressed encoding index outside both tables. The encoding index
		// lives in the high byte of the second entry's word (page at 0x1000).
		bad := append([]byte(nil), raw...)
		bad[cuUnwindOff+0x1000+12+4+3] = 0x7f // second entry, index byte 127
		_, berr = buildIndexOrError(t, bad)
		c.Check(berr != nil && containsText(berr.Error(), "section offset"), "bad encoding index error: %v", berr)

		// 3. Continuation as the very first page entry.
		bad = append([]byte(nil), raw...)
		bad[cuUnwindOff+0x1000+12+3] = 0x80 // first entry gets IS_NOT_FUNCTION_START
		_, berr = buildIndexOrError(t, bad)
		c.Check(berr != nil, "head continuation rejected: %v", berr)

		// 4. STACK_IMMD with a zero stack size.
		bad = append([]byte(nil), raw...)
		binary.LittleEndian.PutUint32(bad[cuUnwindOff+cuCommonSecOff+4:], 0x02000c5b) // size field 0
		_, berr = buildIndexOrError(t, bad)
		c.Check(berr != nil && containsText(berr.Error(), "stack size"), "undersized IMMD rejected: %v", berr)

		// 5. RBP_FRAME with a duplicated register code.
		bad = append([]byte(nil), raw...)
		binary.LittleEndian.PutUint32(bad[cuUnwindOff+cuCommonSecOff:], 0x01020249) // offset 2, slots RBX, RBX
		_, berr = buildIndexOrError(t, bad)
		c.Check(berr != nil && containsText(berr.Error(), "more than once"), "duplicate RBP slot rejected: %v", berr)

		// 6. DWARF reference when __eh_frame is absent.
		noEH, err := buildCUImage(func() cuImageSpec {
			s := spec
			s.ehFrame = false
			return s
		}())
		c.Check(err == nil, "build no-eh image: %v", err)
		_, berr = buildIndexOrError(t, noEH)
		c.Check(berr != nil && containsText(berr.Error(), "__eh_frame"), "missing eh_frame rejected: %v", berr)
	})
}

func TestCompactSources(t *testing.T) {
	t.Parallel()
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			slotOff      = uint32(0x1800)
			pointer      = uint64(0x1122334455667788)
			lsdaImage    = uint32(0x2000)
			lsdaEncoding = uint32(0x52020000) // HAS_LSDA + personality 1 + STACK_IMMD 16
			indEncoding  = uint32(0x03102400) // rel 0x10, adjust +8, one RBX
		)
		slotRaw := make([]byte, 8)
		binary.LittleEndian.PutUint64(slotRaw, pointer)
		spec := cuImageSpec{
			common:        []uint32{lsdaEncoding, indEncoding},
			personalities: []int32{int32(slotOff)}, // base + slotOff names the indirect slot
			slotImageOff:  slotOff,
			slotRaw:       slotRaw,
			immediates:    []cuImmediate{{imageOff: 0x1210, value: 0x30}},
			levels: []cuLevelSpec{
				{
					lsda: []cuLSDASpec{{funcOff: 0x1000, lsdaOff: lsdaImage}},
					page: cuPageSpec{
						start: 0x1000,
						kind:  2,
						regular: []cuRegularEntry{
							{off: 0x1000, encoding: lsdaEncoding},
							{off: 0x1100, continuation: true, encoding: lsdaEncoding},
							{off: 0x1200, encoding: indEncoding},
						},
					},
				},
			},
			sentinelEnd: 0x1400,
		}
		raw, err := buildCUImage(spec)
		c.Check(err == nil, "build source image: %v", err)

		reader := &releasableReaderAt{b: append([]byte(nil), raw...)}
		f, err := NewFile(reader)
		c.Check(err == nil, "NewFile: %v", err)
		idx, err := f.BuildCompactUnwindIndex()
		c.Check(err == nil, "build index: %v", err)

		// Entry provenance: section offset, file offset, VM and owned raw bytes.
		entries := idx.Entries()
		c.Check(len(entries) == 3, "entry count = %d", len(entries))
		sec := f.Section("__TEXT", "__unwind_info")
		c.Check(sec != nil, "unwind section present")
		for i, e := range entries {
			wantFile := uint64(sec.Offset) + uint64(e.Entry.SectionOffset)
			c.Check(e.Entry.FileOffset == wantFile, "entry %d file offset = %#x want %#x", i, e.Entry.FileOffset, wantFile)
			c.Check(e.Entry.VMAddress == sec.Addr+uint64(e.Entry.SectionOffset), "entry %d VM address", i)
			c.Check(bytes.Equal(e.Entry.Raw, raw[wantFile:wantFile+uint64(len(e.Entry.Raw))]), "entry %d raw bytes match file", i)
			// Encoding word provenance is retained separately.
			encFile := uint64(sec.Offset) + uint64(e.EncodingSource.Word.SectionOffset)
			c.Check(e.EncodingSource.Word.FileOffset == encFile, "entry %d encoding file offset", i)
			c.Check(bytes.Equal(e.EncodingSource.Word.Raw, raw[encFile:encFile+4]), "entry %d encoding raw bytes", i)
		}

		// LSDA keyed on the logical function start, visible from the continuation.
		head, lerr := idx.Lookup(cuTestBase + 0x1000)
		c.Check(lerr == nil, "lookup LSDA head: %v", lerr)
		cont, lerr := idx.Lookup(cuTestBase + 0x1100)
		c.Check(lerr == nil, "lookup LSDA continuation: %v", lerr)
		c.Check(head.HasLSDA && cont.HasLSDA, "HAS_LSDA flag on both ranges")
		c.Check(cont.FunctionStart == cuTestBase+0x1000, "continuation keeps logical start for LSDA key")
		c.Check(head.LSDA != nil && cont.LSDA != nil, "both ranges resolve the LSDA")
		c.Check(head.LSDA.VMAddress == cuTestBase+uint64(lsdaImage), "LSDA preferred VM address")
		c.Check(head.LSDA.TargetSectionOffset == lsdaImage, "LSDA image-relative target")
		lsdaRecordOff := uint64(sec.Offset) + uint64(head.LSDA.Record.SectionOffset)
		c.Check(bytes.Equal(head.LSDA.Record.Raw, raw[lsdaRecordOff:lsdaRecordOff+8]), "LSDA record raw bytes")
		c.Check(binary.LittleEndian.Uint32(head.LSDA.Record.Raw) == 0x1000, "LSDA record key is the logical start")

		// Personality: indirect slot address, slot file position, raw pointer.
		pers := idx.Personalities()
		c.Check(len(pers) == 1, "personality count = %d", len(pers))
		c.Check(pers[0].Index == 1 && pers[0].Delta == int32(slotOff), "personality index and signed delta")
		c.Check(pers[0].SlotVMAddress == cuTestBase+uint64(slotOff), "slot VM = %#x", pers[0].SlotVMAddress)
		c.Check(pers[0].SlotFileOffset == uint64(slotOff), "slot file offset")
		c.Check(bytes.Equal(pers[0].Slot.Raw, slotRaw), "slot raw 8 bytes retained")
		c.Check(pers[0].RawPointer == pointer, "unconverted raw pointer = %#x", pers[0].RawPointer)
		c.Check(pers[0].Item.SectionOffset == cuPersSecOff, "personality array item section offset")

		// IND immediate provenance through the File mapping.
		ind, lerr := idx.Lookup(cuTestBase + 0x1200)
		c.Check(lerr == nil, "lookup IND: %v", lerr)
		c.Check(ind.Rule.Immediate.VMAddress == cuTestBase+0x1210, "IND immediate VM")
		c.Check(ind.Rule.Immediate.FileOffset == 0x1210, "IND immediate file offset")
		c.Check(bytes.Equal(ind.Rule.Immediate.Raw[:], []byte{0x30, 0, 0, 0}), "IND immediate raw bytes")
		c.Check(ind.Rule.Immediate.StackSize == 0x38, "IND size 0x30 + 8")

		// Release the File and all backing bytes: the index must still answer.
		c.Check(f.Close() == nil, "close file")
		reader.released = true
		raw = nil
		again, lerr := idx.Lookup(cuTestBase + 0x1100)
		c.Check(lerr == nil, "lookup after release: %v", lerr)
		c.Check(again.FunctionStart == cuTestBase+0x1000 && again.RangeEnd == cuTestBase+0x1200, "post-release range identity")
		c.Check(again.LSDA.VMAddress == cuTestBase+uint64(lsdaImage), "post-release LSDA")
		c.Check(idx.Personalities()[0].RawPointer == pointer, "post-release personality slot")
		c.Check(bytes.Equal(idx.Entries()[2].Rule.Immediate.Raw[:], []byte{0x30, 0, 0, 0}), "post-release immediate bytes")

		// Corruption: unknown page kind, error names the page's section offset.
		// A single level stores its page at section offset 0x1000.
		bad, err := buildCUImage(spec)
		c.Check(err == nil, "rebuild image: %v", err)
		binary.LittleEndian.PutUint32(bad[cuUnwindOff+0x1000:], 9)
		_, berr := buildIndexOrError(t, bad)
		c.Check(berr != nil && containsText(berr.Error(), "0x1000"), "page-kind error names offset: %v", berr)

		// HAS_LSDA with the record keyed at the continuation start must fail:
		// exact logical-start match is required, no nearest-predecessor guess.
		mismatch, err := buildCUImage(func() cuImageSpec {
			s := spec
			s.levels = append([]cuLevelSpec(nil), spec.levels...) // deep copy: do not clobber spec
			s.levels[0].lsda = []cuLSDASpec{{funcOff: 0x1100, lsdaOff: lsdaImage}}
			return s
		}())
		c.Check(err == nil, "build mismatch image: %v", err)
		_, berr = buildIndexOrError(t, mismatch)
		c.Check(berr != nil && containsText(berr.Error(), "LSDA"), "inexact LSDA key rejected: %v", berr)

		// Personality index beyond the array is a hard, offset-bearing error.
		// Regular pages keep the word inline, so corrupt the page entry itself.
		badPers, err := buildCUImage(spec)
		c.Check(err == nil, "build bad-personality image: %v", err)
		binary.LittleEndian.PutUint32(badPers[cuUnwindOff+0x1000+8+4:], 0x62020000) // personality field 2, array has 1
		_, berr = buildIndexOrError(t, badPers)
		c.Check(berr != nil && containsText(berr.Error(), "personality index"), "bad personality index rejected: %v", berr)
	})
}

func buildIndexOrError(t *testing.T, raw []byte) (*CompactUnwindIndex, error) {
	t.Helper()
	f, err := NewFile(bytes.NewReader(raw))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	defer f.Close()
	return f.BuildCompactUnwindIndex()
}

func savedEqual(got, want []CompactSavedRegister) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i].Register != want[i].Register || got[i].Slot.OffsetFromCFA != want[i].Slot.OffsetFromCFA {
			return false
		}
	}
	return true
}

func containsText(haystack, needle string) bool {
	return bytes.Contains([]byte(haystack), []byte(needle))
}
