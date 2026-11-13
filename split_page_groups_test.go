package macho

import (
	"bytes"
	"encoding/binary"
	"testing"

	"github.com/blacktop/go-macho/pkg/trie"
	"github.com/blacktop/go-macho/types"
)

// Synthetic image layout used by TestSplitPageGroups.
//
//	__TEXT 0x100000-0x104000 (file 0x0000-0x3000)
//	  section 1 __text  vm 0x100800 size 0x600 file 0x0800
//	  section 2 __const vm 0x102000 size 0x080 file 0x2000  (target A)
//	  section 3 __data  vm 0x102080 size 0xf00 file 0x2080  (targets B, C)
//	__DATA 0x104000-0x106000 (zero fill only)
//	  section 4 __bss   vm 0x104000 size 0x1000 zerofill    (target D)
//
// A (0x102000) and B (0x102080) share page 0x102000 in the original image.
const (
	spgTextAddr  = 0x100800
	spgConstAddr = 0x102000
	spgDataAddr  = 0x102080
	spgBSSAddr   = 0x104000
	spgSplitOff  = 0x3800
	spgFileSize  = 0x4000
)

// spgRawRef is one LC_SEGMENT_SPLIT_INFO v2 reference in section coordinates.
type spgRawRef struct {
	kind              types.SplitInfoKind
	fromSect, fromOff uint64
	toSect, toOff     uint64
}

// adrpSPG encodes ADRP Xd targeting targetPage from an instruction at srcAddr.
func adrpSPG(rd uint32, srcAddr, targetPage uint64) uint32 {
	srcPage := (srcAddr &^ uint64(0xfff)) >> 12
	pages := int64(targetPage>>12) - int64(srcPage)
	imm := uint32(pages) & 0x1fffff
	return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd
}

func addImmSPG(rd, rn uint32, imm uint32) uint32 {
	return 0x91000000 | (imm << 10) | (rn << 5) | rd
}

func ldrSPG(rt, rn uint32, byteOffset uint32) uint32 {
	return 0xf9400000 | ((byteOffset / 8) << 10) | (rn << 5) | rt
}

func movSPG(rd, rm uint32) uint32 { return 0xaa0003e0 | (rm << 16) | rd }

func bSPG(pcOffset int) uint32 {
	off := uint32(pcOffset / 4)
	return 0x14000000 | (off & 0x03ffffff)
}

func cbnzSPG(rt uint32, pcOffset int) uint32 {
	off := uint32(pcOffset/4) & 0x7ffff
	return 0xb5000000 | (off << 5) | rt
}

func cbzSPG(rt uint32, pcOffset int) uint32 {
	off := uint32(pcOffset/4) & 0x7ffff
	return 0xb4000000 | (off << 5) | rt
}

// encodeSplitInfoV2 builds a v2 payload, accumulating target and source
// offsets as deltas exactly like the production format.
func encodeSplitInfoV2(refs []spgRawRef) []byte {
	type pair struct{ from, to uint64 }
	pairRefs := make(map[pair][]spgRawRef)
	var pairs []pair
	for _, ref := range refs {
		p := pair{ref.fromSect, ref.toSect}
		if _, ok := pairRefs[p]; !ok {
			pairs = append(pairs, p)
		}
		pairRefs[p] = append(pairRefs[p], ref)
	}
	var buf bytes.Buffer
	buf.WriteByte(types.DYLD_CACHE_ADJ_V2_FORMAT)
	trie.EncodeUleb128(&buf, uint64(len(pairs)))
	for _, p := range pairs {
		group := pairRefs[p]
		trie.EncodeUleb128(&buf, p.from)
		trie.EncodeUleb128(&buf, p.to)

		type sources struct {
			kind types.SplitInfoKind
			off  uint64
		}
		targets := make(map[uint64][]sources)
		var targetOffsets []uint64
		for _, ref := range group {
			if _, ok := targets[ref.toOff]; !ok {
				targetOffsets = append(targetOffsets, ref.toOff)
			}
			targets[ref.toOff] = append(targets[ref.toOff], sources{ref.kind, ref.fromOff})
		}
		trie.EncodeUleb128(&buf, uint64(len(targetOffsets)))
		var lastToOff uint64
		for _, toOff := range targetOffsets {
			trie.EncodeUleb128(&buf, toOff-lastToOff)
			lastToOff = toOff

			kinds := make(map[types.SplitInfoKind][]uint64)
			var kindList []types.SplitInfoKind
			for _, s := range targets[toOff] {
				if _, ok := kinds[s.kind]; !ok {
					kindList = append(kindList, s.kind)
				}
				kinds[s.kind] = append(kinds[s.kind], s.off)
			}
			trie.EncodeUleb128(&buf, uint64(len(kindList)))
			for _, kind := range kindList {
				offs := kinds[kind]
				trie.EncodeUleb128(&buf, uint64(kind))
				trie.EncodeUleb128(&buf, uint64(len(offs)))
				var lastFromOff uint64
				for _, off := range offs {
					trie.EncodeUleb128(&buf, off-lastFromOff)
					lastFromOff = off
				}
			}
		}
	}
	return buf.Bytes()
}

// buildSplitPageImage assembles an in-memory ARM64 Mach-O with the code at
// textOffsets and the given split-info references.
func buildSplitPageImage(t *testing.T, code map[uint64]uint32, refs []spgRawRef) (*File, []byte) {
	t.Helper()
	var textName, constName, dataName, bssName, textSeg, dataSeg [16]byte
	copy(textName[:], "__text")
	copy(constName[:], "__const")
	copy(dataName[:], "__data")
	copy(bssName[:], "__bss")
	copy(textSeg[:], "__TEXT")
	copy(dataSeg[:], "__DATA")

	var loads bytes.Buffer
	writeLoad := func(v any) {
		t.Helper()
		if err := binary.Write(&loads, binary.LittleEndian, v); err != nil {
			t.Fatal(err)
		}
	}
	writeLoad(types.Segment64{ // __TEXT, 3 sections
		LoadCmd: types.LC_SEGMENT_64, Len: 72 + 3*80, Name: textSeg,
		Addr: 0x100000, Memsz: 0x4000, Offset: 0, Filesz: 0x3000, Nsect: 3,
	})
	writeLoad(types.Section64{
		Name: textName, Seg: textSeg, Addr: spgTextAddr, Size: 0x600,
		Offset: 0x800, Align: 2, Flags: types.PURE_INSTRUCTIONS,
	})
	writeLoad(types.Section64{
		Name: constName, Seg: textSeg, Addr: spgConstAddr, Size: 0x80,
		Offset: 0x2000, Align: 2,
	})
	writeLoad(types.Section64{
		Name: dataName, Seg: textSeg, Addr: spgDataAddr, Size: 0xf00,
		Offset: 0x2080, Align: 2,
	})
	writeLoad(types.Segment64{ // __DATA, one zerofill section
		LoadCmd: types.LC_SEGMENT_64, Len: 72 + 80, Name: dataSeg,
		Addr: spgBSSAddr, Memsz: 0x2000, Offset: 0x4000, Filesz: 0, Nsect: 1,
	})
	writeLoad(types.Section64{
		Name: bssName, Seg: dataSeg, Addr: spgBSSAddr, Size: 0x1000,
		Align: 2, Flags: types.Zerofill,
	})
	payload := encodeSplitInfoV2(refs)
	writeLoad(types.SegmentSplitInfoCmd{
		LoadCmd: types.LC_SEGMENT_SPLIT_INFO, Len: 16,
		Offset: spgSplitOff, Size: uint32(len(payload)),
	})

	data := make([]byte, spgFileSize)
	header := types.FileHeader{
		Magic: types.Magic64, CPU: types.CPUArm64, Type: types.MH_DYLIB,
		NCommands: 3, SizeCommands: uint32(loads.Len()),
	}
	n := header.Put(data, binary.LittleEndian)
	copy(data[n:], loads.Bytes())
	for off, insn := range code {
		binary.LittleEndian.PutUint32(data[0x800+off:], insn)
	}
	copy(data[spgSplitOff:], payload)

	f, err := NewFile(bytes.NewReader(data))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	t.Cleanup(func() { f.Close() })
	return f, data
}

// splitPageFixtureCode is the shared program. Each comment block is one
// scenario exercised by its own subtest.
func splitPageFixtureCode() map[uint64]uint32 {
	const (
		nop = 0xd503201f
		ret = 0xd65f03c0
	)
	return map[uint64]uint32{
		// func1 @0x00: two consumers of one ADRP base register. The ADD
		// write-back targets x1 so the shared base x0 survives for the LDR.
		0x00: adrpSPG(0, spgTextAddr+0x00, spgConstAddr&^0xfff),
		0x04: addImmSPG(1, 0, 0x000),                 // -> A: section 2 offset 0
		0x08: ldrSPG(2, 0, spgDataAddr-spgConstAddr), // -> B: section 3 offset 0
		0x0c: nop,
		0x10: ret,

		// func2 @0x20: MOV carries the ADRP source into another register.
		0x20: adrpSPG(0, spgTextAddr+0x20, spgConstAddr&^0xfff),
		0x24: movSPG(1, 0),
		0x28: addImmSPG(1, 1, 0),
		0x2c: ret,

		// func3 @0x40: the second ADRP overwrites the first; the consumer
		// must bind to the later instruction.
		0x40: adrpSPG(0, spgTextAddr+0x40, spgConstAddr&^0xfff),
		0x44: adrpSPG(0, spgTextAddr+0x44, spgConstAddr&^0xfff),
		0x48: addImmSPG(0, 0, 0xe80), // -> C: section 3 offset 0xe00 (0x102e80)
		0x4c: ret,

		// func4 @0x60: conditional join of two distinct ADRP sources.
		0x60: adrpSPG(0, spgTextAddr+0x60, spgConstAddr&^0xfff),
		0x64: adrpSPG(1, spgTextAddr+0x64, 0x103000),
		0x68: cbnzSPG(9, 0x74-0x68), // taken path jumps to the second MOV
		0x6c: movSPG(2, 0),          // fall-through path: x2 <- ADRP x0
		0x70: bSPG(0x78 - 0x70),     // skip the taken-path MOV
		0x74: movSPG(2, 1),          // taken path: x2 <- ADRP x1
		0x78: addImmSPG(2, 2, 0),    // ambiguous consumer at the merge
		0x7c: ret,

		// func5 @0x80: an unsupported BL is an analysis barrier.
		0x80: 0x94000000, // BL
		0x84: addImmSPG(0, 0, 0),
		0x88: ret,

		// func6 @0xa0: loop with a back edge; the ADRP source survives.
		0xa0: adrpSPG(0, spgTextAddr+0xa0, spgConstAddr&^0xfff),
		0xa4: movSPG(1, 0),
		0xa8: bSPG(0xb0 - 0xa8),
		0xac: movSPG(1, 0),          // loop body
		0xb0: addImmSPG(1, 1, 0),    // consumer reached from both edges
		0xb4: cbzSPG(10, 0xac-0xb4), // back edge
		0xb8: ret,

		// func7 @0xc0: metadata lies about the target -> inconsistent.
		0xc0: adrpSPG(0, spgTextAddr+0xc0, spgConstAddr&^0xfff),
		0xc4: addImmSPG(0, 0, 0),
		0xc8: ret,

		// func8 @0xe0: targets in a zerofill section and in the header.
		0xe0: adrpSPG(0, spgTextAddr+0xe0, spgBSSAddr&^0xfff),
		0xe4: addImmSPG(0, 0, 0), // -> D: section 4 offset 0
		0xe8: ret,
		0xf0: adrpSPG(0, spgTextAddr+0xf0, 0x100000),
		0xf4: addImmSPG(0, 0, 0), // -> image header: section 0 offset 0
		0xf8: ret,

		// func9 @0x100: an ADD write-back is an ordinary overwrite, so the
		// destination no longer carries the ADRP source.
		0x100: adrpSPG(0, spgTextAddr+0x100, spgConstAddr&^0xfff),
		0x104: addImmSPG(1, 0, 0), // x1 overwritten with a non-ADRP value
		0x108: addImmSPG(2, 1, 0), // consumer on x1: base is not an ADRP page
		0x10c: ret,
	}
}

func splitPageFixtureRefs() []spgRawRef {
	return []spgRawRef{
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x00, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x08, 3, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x20, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x28, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x44, 3, 0xe00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x48, 3, 0xe00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x74, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x84, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0xb0, 2, 0x00},
		// func7 deliberately mismatches: the ADD encodes section 2's page but
		// the metadata claims section 3.
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0xc0, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0xc4, 3, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0xe0, 4, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0xe4, 4, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0xf0, 0, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0xf4, 0, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x100, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x104, 2, 0x00},
		{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x108, 2, 0x00},
	}
}

func splitPageRanges(pairs ...[2]uint64) []SplitPageCodeRange {
	var out []SplitPageCodeRange
	for _, p := range pairs {
		out = append(out, SplitPageCodeRange{SectionIndex: 1, Offset: p[0], Length: p[1]})
	}
	return out
}

// allSplitPageRanges gives each synthetic function its own entry range.
func allSplitPageRanges() []SplitPageCodeRange {
	return splitPageRanges(
		[2]uint64{0x00, 0x14}, [2]uint64{0x20, 0x10}, [2]uint64{0x40, 0x10},
		[2]uint64{0x60, 0x20}, [2]uint64{0x80, 0x0c}, [2]uint64{0xa0, 0x1c},
		[2]uint64{0xc0, 0x0c}, [2]uint64{0xe0, 0x0c}, [2]uint64{0xf0, 0x0c},
		[2]uint64{0x100, 0x10},
	)
}

func findSplitPageRef(t *testing.T, report *SplitPageReport, off uint64) *SplitPageReference {
	t.Helper()
	for _, ref := range report.References {
		if ref.SourceSectionIndex == 1 && ref.SourceSectionOffset == off {
			return ref
		}
	}
	t.Fatalf("no report reference for source offset %#x", off)
	return nil
}

func TestSplitPageGroups(t *testing.T) {
	t.Run("proves shared ADRP group with ADD and scaled LDR", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		add := findSplitPageRef(t, report, 0x04)
		ldr := findSplitPageRef(t, report, 0x08)
		if add.Status != SplitPageStatusCompatible || ldr.Status != SplitPageStatusCompatible {
			t.Fatalf("statuses = %s, %s; want both compatible", add.Status, ldr.Status)
		}
		if add.Group != ldr.Group || add.Group == nil {
			t.Fatal("ADD and LDR consumers were not proved to share one ADRP")
		}
		if add.Group.ADRPSectionOffset != 0x00 || len(add.Group.References) != 2 {
			t.Fatalf("group = %+v, want ADRP at 0x00 with 2 references", add.Group)
		}
		if add.SourceVmAddr != spgTextAddr+0x04 || add.TargetVmAddr != spgConstAddr {
			t.Fatalf("ADD addresses = %#x -> %#x", add.SourceVmAddr, add.TargetVmAddr)
		}
		if ldr.TargetVmAddr != spgDataAddr {
			t.Fatalf("LDR target = %#x, want %#x", ldr.TargetVmAddr, spgDataAddr)
		}
	})

	t.Run("MOV propagates source and second ADRP overwrites the first", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		moved := findSplitPageRef(t, report, 0x28)
		if moved.Status != SplitPageStatusCompatible || moved.Group == nil ||
			moved.Group.ADRPSectionOffset != 0x20 {
			t.Fatalf("MOV consumer = %+v, want compatible group rooted at 0x20", moved)
		}
		overwritten := findSplitPageRef(t, report, 0x48)
		if overwritten.Status != SplitPageStatusCompatible || overwritten.Group == nil ||
			overwritten.Group.ADRPSectionOffset != 0x44 {
			t.Fatalf("overwrite consumer = %+v, want group rooted at the second ADRP 0x44", overwritten)
		}
		if overwritten.TargetSectionOffset != 0xe00 || overwritten.TargetVmAddr != 0x102e80 {
			t.Fatalf("overwrite target = %#x, want 0x102e80", overwritten.TargetVmAddr)
		}
	})

	t.Run("conditional join of distinct ADRPs is ambiguous", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := findSplitPageRef(t, report, 0x74)
		if ref.Status != SplitPageStatusAmbiguous {
			t.Fatalf("status = %s (%s), want ambiguous", ref.Status, ref.Reason)
		}
		if ref.Group != nil || ref.Reason == "" {
			t.Fatalf("ambiguous reference must have no group and a reason, got group=%p reason=%q", ref.Group, ref.Reason)
		}
	})

	t.Run("ADD write-back overwrites the ADRP source", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		// The first ADD at 0x104 still consumes the proved ADRP base x0.
		direct := findSplitPageRef(t, report, 0x104)
		if direct.Status != SplitPageStatusCompatible || direct.Group == nil ||
			direct.Group.ADRPSectionOffset != 0x100 {
			t.Fatalf("direct ADD consumer = %+v, want a compatible group rooted at 0x100", direct)
		}
		// Its write-back made x1 a non-ADRP value, so the second ADD cannot
		// claim an ADRP source and is ambiguous rather than grouped.
		overwritten := findSplitPageRef(t, report, 0x108)
		if overwritten.Status != SplitPageStatusAmbiguous || overwritten.Group != nil {
			t.Fatalf("overwritten base consumer = %+v, want ambiguous with no group", overwritten)
		}
	})

	t.Run("unsupported instruction barrier makes the consumer unknown", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := findSplitPageRef(t, report, 0x84)
		if ref.Status != SplitPageStatusUnknown || ref.Reason == "" {
			t.Fatalf("status = %s reason=%q, want unknown with reason", ref.Status, ref.Reason)
		}
		if ref.Group != nil {
			t.Fatal("unknown reference must not belong to a group")
		}
	})

	t.Run("loop back edge converges on the unique ADRP", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := findSplitPageRef(t, report, 0xb0)
		if ref.Status != SplitPageStatusCompatible || ref.Group == nil ||
			ref.Group.ADRPSectionOffset != 0xa0 {
			t.Fatalf("loop consumer = %+v, want compatible group rooted at 0xa0", ref)
		}
	})

	t.Run("moved sections split the shared page and conflict for every consumer", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		newBases := map[int]uint64{
			2: 0x200000, // target A moves to page 0x200000
			3: 0x201000, // target B moves to page 0x201000
		}
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), newBases)
		if err != nil {
			t.Fatal(err)
		}
		add := findSplitPageRef(t, report, 0x04)
		ldr := findSplitPageRef(t, report, 0x08)
		if add.Status != SplitPageStatusConflict || ldr.Status != SplitPageStatusConflict {
			t.Fatalf("statuses = %s, %s; want both conflict", add.Status, ldr.Status)
		}
		if add.Group != ldr.Group || add.Group.Status != SplitPageStatusConflict {
			t.Fatal("the split must be reported for the whole shared group")
		}
		if add.NewTargetVmAddr != 0x200000 || ldr.NewTargetVmAddr != 0x201000 {
			t.Fatalf("new targets = %#x, %#x", add.NewTargetVmAddr, ldr.NewTargetVmAddr)
		}
		if add.Reason == "" || ldr.Reason == "" {
			t.Fatal("conflict references must explain the page split")
		}
		// The untouched text section keeps its old addresses.
		if add.NewSourceVmAddr != add.SourceVmAddr {
			t.Fatalf("source moved although section 1 was not remapped: %#x -> %#x", add.SourceVmAddr, add.NewSourceVmAddr)
		}
	})

	t.Run("ADRP page distance beyond 21 bits conflicts", func(t *testing.T) {
		refs := []spgRawRef{
			{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x00, 2, 0x00},
			{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 2, 0x00},
		}
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), refs)
		report, err := f.AnalyzeSplitPageGroups(
			splitPageRanges([2]uint64{0x00, 0x14}),
			map[int]uint64{2: 0x100000000000},
		)
		if err != nil {
			t.Fatal(err)
		}
		ref := findSplitPageRef(t, report, 0x04)
		if ref.Status != SplitPageStatusConflict {
			t.Fatalf("status = %s, want conflict", ref.Status)
		}
		if ref.Group == nil || ref.Group.Status != SplitPageStatusConflict {
			t.Fatal("the group must inherit the overflow conflict")
		}
	})

	t.Run("metadata that the ADRP and OFF12 cannot reconstruct is inconsistent", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := findSplitPageRef(t, report, 0xc4)
		if ref.Status != SplitPageStatusInconsistent || ref.Reason == "" {
			t.Fatalf("status = %s reason=%q, want inconsistent with reason", ref.Status, ref.Reason)
		}
		if ref.Group == nil || ref.Group.Status != SplitPageStatusInconsistent {
			t.Fatal("the owning group must inherit the inconsistent verdict")
		}
	})

	t.Run("zerofill and image-header targets are accepted", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		report, err := f.AnalyzeSplitPageGroups(allSplitPageRanges(), nil)
		if err != nil {
			t.Fatal(err)
		}
		bss := findSplitPageRef(t, report, 0xe4)
		if bss.Status != SplitPageStatusCompatible || bss.TargetSectionIndex != 4 ||
			bss.TargetVmAddr != spgBSSAddr {
			t.Fatalf("zerofill target reference = %+v", bss)
		}
		header := findSplitPageRef(t, report, 0xf4)
		if header.Status != SplitPageStatusCompatible || header.TargetSectionIndex != 0 ||
			header.TargetVmAddr != f.GetBaseAddress() {
			t.Fatalf("header target reference = %+v, base=%#x", header, f.GetBaseAddress())
		}
	})

	t.Run("rejects malformed ranges and out-of-range metadata", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		if _, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 1, Offset: 0, Length: 6},
		}, nil); err == nil {
			t.Fatal("expected error for length that is not a multiple of four")
		}
		if _, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 1, Offset: 0, Length: 0x10},
			{SectionIndex: 1, Offset: 0xc, Length: 0x10},
		}, nil); err == nil {
			t.Fatal("expected error for overlapping ranges")
		}
		if _, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 0, Offset: 0, Length: 4},
		}, nil); err == nil {
			t.Fatal("expected error for section zero used as a code source")
		}
		if _, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 99, Offset: 0, Length: 4},
		}, nil); err == nil {
			t.Fatal("expected error for an unknown section index")
		}
		// Target beyond the declared section size.
		bad, _ := buildSplitPageImage(t, splitPageFixtureCode(), []spgRawRef{
			{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 2, 0x100},
		})
		if _, err := bad.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 1, Offset: 0, Length: 0x10},
		}, nil); err == nil {
			t.Fatal("expected error for a target past the section size")
		}
	})

	t.Run("rejects conflicting metadata and merges exact duplicates", func(t *testing.T) {
		conflicting, _ := buildSplitPageImage(t, splitPageFixtureCode(), []spgRawRef{
			{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 2, 0x00},
			{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 3, 0x00},
		})
		if _, err := conflicting.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 1, Offset: 0, Length: 0x10},
		}, nil); err == nil {
			t.Fatal("expected error for conflicting metadata on one source")
		}
		duplicated, _ := buildSplitPageImage(t, splitPageFixtureCode(), []spgRawRef{
			{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x00, 2, 0x00},
			{types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, 1, 0x00, 2, 0x00},
			{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 2, 0x00},
			{types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, 1, 0x04, 2, 0x00},
		})
		report, err := duplicated.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: 1, Offset: 0, Length: 0x10},
		}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(report.References) != 1 || len(report.Groups) != 1 {
			t.Fatalf("got %d references and %d groups, want duplicates merged to 1/1",
				len(report.References), len(report.Groups))
		}
	})

	t.Run("repeated calls are deterministic and accumulate no state", func(t *testing.T) {
		f, _ := buildSplitPageImage(t, splitPageFixtureCode(), splitPageFixtureRefs())
		ranges := allSplitPageRanges()
		first, err := f.AnalyzeSplitPageGroups(ranges, map[int]uint64{2: 0x200000, 3: 0x201000})
		if err != nil {
			t.Fatal(err)
		}
		second, err := f.AnalyzeSplitPageGroups(ranges, map[int]uint64{2: 0x200000, 3: 0x201000})
		if err != nil {
			t.Fatal(err)
		}
		if len(first.References) != len(second.References) ||
			len(first.Groups) != len(second.Groups) {
			t.Fatal("repeated analysis changed size")
		}
		for i := range first.References {
			a, b := first.References[i], second.References[i]
			if a.Status != b.Status || a.SourceSectionOffset != b.SourceSectionOffset ||
				a.NewTargetVmAddr != b.NewTargetVmAddr || a.Reason != b.Reason {
				t.Fatalf("reference %d differs between calls: %+v vs %+v", i, a, b)
			}
		}
	})

	t.Run("non-ARM64 images are rejected", func(t *testing.T) {
		f := &File{FileTOC: FileTOC{Sections: []*types.Section{{}}}}
		if _, err := f.AnalyzeSplitPageGroups(nil, nil); err == nil {
			t.Fatal("expected an error for a non-ARM64 file")
		}
	})
}
