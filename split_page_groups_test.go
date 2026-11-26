package macho

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"strings"
	"testing"

	"github.com/blacktop/go-macho/pkg/trie"
	"github.com/blacktop/go-macho/types"
)

const (
	spTextSection     uint64 = 1
	spConstASection   uint64 = 2
	spConstBSection   uint64 = 3
	spZerofillSection uint64 = 4

	spTextAddr     uint64 = 0x100400
	spConstAAddr   uint64 = 0x101000
	spConstBAddr   uint64 = 0x101800
	spZerofillAddr uint64 = 0x200000
)

type spSectionSpec struct {
	name   string
	addr   uint64
	size   uint64
	offset uint32
	flags  types.SectionFlag
}

type spSegmentSpec struct {
	name     string
	addr     uint64
	memsz    uint64
	filesz   uint64
	offset   uint64
	sections []spSectionSpec
}

type spRef struct {
	kind        types.SplitInfoKind
	fromSection uint64
	fromOffset  uint64
	toSection   uint64
	toOffset    uint64
}

func spEncodeULEB(buf *bytes.Buffer, v uint64) { trie.EncodeUleb128(buf, v) }

// spEncodeSplitInfo emits one top-level group per reference. The v2 reader
// resets the section and offset deltas for every top-level entry, so this is
// the simplest way to describe arbitrary reference lists, including duplicates.
func spEncodeSplitInfo(refs []spRef) []byte {
	var buf bytes.Buffer
	buf.WriteByte(types.DYLD_CACHE_ADJ_V2_FORMAT)
	spEncodeULEB(&buf, uint64(len(refs)))
	for _, ref := range refs {
		spEncodeULEB(&buf, ref.fromSection)
		spEncodeULEB(&buf, ref.toSection)
		spEncodeULEB(&buf, 1) // toOffsetCount
		spEncodeULEB(&buf, ref.toOffset)
		spEncodeULEB(&buf, 1) // fromOffsetCount
		spEncodeULEB(&buf, uint64(ref.kind))
		spEncodeULEB(&buf, 1) // fromSectDeltaCount
		spEncodeULEB(&buf, ref.fromOffset)
	}
	buf.WriteByte(0)
	return buf.Bytes()
}

func newSplitPageFile(t *testing.T, code []uint32, refs []spRef) *File {
	t.Helper()
	specs := []spSegmentSpec{{
		name: "__TEXT", addr: 0x100000, memsz: 0x2000, filesz: 0x2000, offset: 0,
		sections: []spSectionSpec{
			{name: "__text", addr: spTextAddr, size: 0x400, offset: 0x400, flags: types.PURE_INSTRUCTIONS},
			{name: "__const_a", addr: spConstAAddr, size: 0x400, offset: 0x1000},
			{name: "__const_b", addr: spConstBAddr, size: 0x400, offset: 0x1800},
		},
	}, {
		name: "__DATA_ZF", addr: spZerofillAddr, memsz: 0x1000, filesz: 0, offset: 0x2000,
		sections: []spSectionSpec{
			{name: "__bss", addr: spZerofillAddr, size: 0x1000, offset: 0, flags: types.Zerofill},
		},
	}}
	const payloadOffset = 0x3000
	payload := spEncodeSplitInfo(refs)

	var loads bytes.Buffer
	for _, seg := range specs {
		var segName [16]byte
		copy(segName[:], seg.name)
		load := types.Segment64{
			LoadCmd: types.LC_SEGMENT_64,
			Len:     uint32(binary.Size(types.Segment64{})) + uint32(binary.Size(types.Section64{}))*uint32(len(seg.sections)),
			Name:    segName,
			Addr:    seg.addr,
			Memsz:   seg.memsz,
			Filesz:  seg.filesz,
			Offset:  seg.offset,
			Maxprot: 7,
			Prot:    5,
			Nsect:   uint32(len(seg.sections)),
		}
		if err := binary.Write(&loads, binary.LittleEndian, load); err != nil {
			t.Fatal(err)
		}
		for _, spec := range seg.sections {
			var name, segNameCopy [16]byte
			copy(name[:], spec.name)
			copy(segNameCopy[:], seg.name)
			if err := binary.Write(&loads, binary.LittleEndian, types.Section64{
				Name:   name,
				Seg:    segNameCopy,
				Addr:   spec.addr,
				Size:   spec.size,
				Offset: spec.offset,
				Align:  2,
				Flags:  spec.flags,
			}); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := binary.Write(&loads, binary.LittleEndian, types.SegmentSplitInfoCmd{
		LoadCmd: types.LC_SEGMENT_SPLIT_INFO,
		Len:     uint32(binary.Size(types.SegmentSplitInfoCmd{})),
		Offset:  payloadOffset,
		Size:    uint32(len(payload)),
	}); err != nil {
		t.Fatal(err)
	}

	header := types.FileHeader{
		Magic:        types.Magic64,
		CPU:          types.CPUArm64,
		Type:         types.MH_DYLIB,
		NCommands:    uint32(len(specs)) + 1,
		SizeCommands: uint32(loads.Len()),
	}
	data := make([]byte, payloadOffset+len(payload))
	n := header.Put(data, binary.LittleEndian)
	copy(data[n:], loads.Bytes())
	for i, instruction := range code {
		binary.LittleEndian.PutUint32(data[0x400+4*i:], instruction)
	}
	copy(data[payloadOffset:], payload)

	f, err := NewFile(bytes.NewReader(data))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { f.Close() })
	return f
}

func encADRP(rd uint32, pc, targetPage uint64) uint32 {
	deltaPages := int64(targetPage>>12) - int64((pc&^uint64(0xFFF))>>12)
	imm := uint64(deltaPages) & 0x1FFFFF
	immlo := uint32(imm&0x3) << 29
	immhi := uint32((imm>>2)&0x7FFFF) << 5
	return 0x90000000 | immlo | immhi | (rd & 0x1F)
}

func encADD(rd, rn, imm uint32) uint32 {
	return 0x91000000 | ((imm & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rd & 0x1F)
}

func encMOV(rd, rm uint32) uint32 {
	return 0xAA0003E0 | ((rm & 0x1F) << 16) | (rd & 0x1F)
}

func encLDRX(rt, rn, byteOffset uint32) uint32 {
	return 0xF9400000 | (((byteOffset / 8) & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)
}

func encSTRX(rt, rn, byteOffset uint32) uint32 {
	return 0xF9000000 | (((byteOffset / 8) & 0xFFF) << 10) | ((rn & 0x1F) << 5) | (rt & 0x1F)
}

func encRET() uint32 { return 0xD65F03C0 }

func encB(pc, target uint64) uint32 {
	delta := (int64(target) - int64(pc)) >> 2
	return 0x14000000 | (uint32(delta) & 0x03FFFFFF)
}

func encCBZ64(rt uint32, pc, target uint64) uint32 {
	delta := (int64(target) - int64(pc)) >> 2
	return 0xB4000000 | ((uint32(delta) & 0x7FFFF) << 5) | (rt & 0x1F)
}

func encCBNZ64(rt uint32, pc, target uint64) uint32 {
	delta := (int64(target) - int64(pc)) >> 2
	return 0xB5000000 | ((uint32(delta) & 0x7FFFF) << 5) | (rt & 0x1F)
}

func spFindGroup(t *testing.T, analysis *SplitPageAnalysis, adrpOffset uint64) *SplitPageGroup {
	t.Helper()
	for _, group := range analysis.Groups {
		if group.ADRPSectionOffset == adrpOffset {
			return group
		}
	}
	t.Fatalf("no group for ADRP at section offset %#x (groups=%d)", adrpOffset, len(analysis.Groups))
	return nil
}

func spFindRef(t *testing.T, analysis *SplitPageAnalysis, sourceOffset uint64) *SplitPageReference {
	t.Helper()
	for _, ref := range analysis.References {
		if ref.SourceOffset == sourceOffset {
			return ref
		}
	}
	t.Fatalf("no reference for source offset %#x (refs=%d)", sourceOffset, len(analysis.References))
	return nil
}

func spRequireStatus(t *testing.T, ref *SplitPageReference, want SplitPageStatus) {
	t.Helper()
	if ref.Status != want {
		t.Fatalf("reference %#x status = %q (reason %q), want %q", ref.SourceOffset, ref.Status, ref.Reason, want)
	}
}

func TestSplitPageGroups(t *testing.T) {
	t.Run("separate_registers_keep_own_adrp", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encADRP(1, pc+4, spConstAAddr),
			encADD(2, 0, 0x20),
			encADD(3, 1, 0x840),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, fromSection: spTextSection, fromOffset: 0, toSection: spConstASection, toOffset: 0},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_ADRP, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0xc, toSection: spConstBSection, toOffset: 0x40},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x14}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 2 {
			t.Fatalf("groups = %d, want 2", len(analysis.Groups))
		}
		first := spFindGroup(t, analysis, 0)
		if len(first.Consumers) != 1 || first.Consumers[0].SourceOffset != 8 {
			t.Fatalf("ADRP at 0 consumers = %+v, want only the ADD at 8", first.Consumers)
		}
		spRequireStatus(t, first.Consumers[0], SplitPageCompatible)
		second := spFindGroup(t, analysis, 4)
		if len(second.Consumers) != 1 || second.Consumers[0].SourceOffset != 0xc {
			t.Fatalf("ADRP at 4 consumers = %+v, want only the ADD at 0xc", second.Consumers)
		}
		spRequireStatus(t, second.Consumers[0], SplitPageCompatible)
		if len(analysis.References) != 2 {
			t.Fatalf("references = %d, want 2 OFF12 references", len(analysis.References))
		}
	})

	t.Run("mov_propagates_provenance", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encMOV(5, 0), encADD(2, 5, 0x20), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 1 {
			t.Fatalf("groups = %d, want 1", len(analysis.Groups))
		}
		group := spFindGroup(t, analysis, 0)
		if len(group.Consumers) != 1 || group.Consumers[0].SourceOffset != 8 {
			t.Fatalf("group consumers = %+v", group.Consumers)
		}
		spRequireStatus(t, group.Consumers[0], SplitPageCompatible)
	})

	t.Run("overwrite_clears_provenance", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encMOV(5, 0), encMOV(5, 6), encADD(2, 5, 0x20), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0xc, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x14}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 0 {
			t.Fatalf("groups = %d, want 0 after the base register was overwritten", len(analysis.Groups))
		}
		ref := spFindRef(t, analysis, 0xc)
		spRequireStatus(t, ref, SplitPageUnknown)
		if !strings.Contains(ref.Reason, "no ADRP provenance") {
			t.Fatalf("reason = %q, want a no-provenance explanation", ref.Reason)
		}
	})

	t.Run("conditional_merge_is_ambiguous", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encCBZ64(1, pc+4, pc+0xc),
			encADRP(0, pc+8, spConstAAddr),
			encADD(2, 0, 0x20),
			encRET(),
		}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0xc, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x14}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 0 {
			t.Fatalf("groups = %d, want 0 for an ambiguous merge", len(analysis.Groups))
		}
		ref := spFindRef(t, analysis, 0xc)
		spRequireStatus(t, ref, SplitPageAmbiguous)
		if !strings.Contains(ref.Reason, "ambiguous") {
			t.Fatalf("reason = %q, want an ambiguity explanation", ref.Reason)
		}
	})

	t.Run("backedge_carries_provenance", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encADD(2, 0, 0x20),
			encCBNZ64(1, pc+8, pc+4), // loop back to the ADD
			encADD(3, 0, 0x24),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0xc, toSection: spConstASection, toOffset: 0x24},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x14}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 1 {
			t.Fatalf("groups = %d, want one group across the backedge", len(analysis.Groups))
		}
		group := spFindGroup(t, analysis, 0)
		if len(group.Consumers) != 2 {
			t.Fatalf("consumers = %d, want 2", len(group.Consumers))
		}
		for _, consumer := range group.Consumers {
			spRequireStatus(t, consumer, SplitPageCompatible)
		}
	})

	t.Run("same_old_page_splits_into_conflict", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encADD(2, 0, 0x20),
			encADD(3, 0, 0x840),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstBSection, toOffset: 0x40},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups(
			[]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}},
			map[uint64]uint64{spConstASection: 0x301000, spConstBSection: 0x303000},
		)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 1 {
			t.Fatalf("groups = %d, want 1", len(analysis.Groups))
		}
		group := analysis.Groups[0]
		if group.Status != SplitPageConflict {
			t.Fatalf("group status = %q (reason %q), want conflict", group.Status, group.Reason)
		}
		if len(group.Consumers) != 2 {
			t.Fatalf("group consumers = %d, want both conflicting consumers", len(group.Consumers))
		}
		seen := map[uint64]bool{}
		for _, consumer := range group.Consumers {
			if consumer.Status != SplitPageConflict {
				t.Fatalf("consumer %#x status = %q, want conflict", consumer.SourceOffset, consumer.Status)
			}
			seen[consumer.NewTargetVmAddr&^uint64(0xFFF)] = true
		}
		if !seen[0x301000] || !seen[0x303000] {
			t.Fatalf("conflicting consumer pages = %v, want 0x301000 and 0x303000", seen)
		}
		if spFindRef(t, analysis, 4).NewTargetVmAddr != 0x301020 {
			t.Fatalf("first new target = %#x, want 0x301020", spFindRef(t, analysis, 4).NewTargetVmAddr)
		}
		if spFindRef(t, analysis, 8).NewTargetVmAddr != 0x303040 {
			t.Fatalf("second new target = %#x, want 0x303040", spFindRef(t, analysis, 8).NewTargetVmAddr)
		}
	})

	t.Run("same_new_page_stays_compatible", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encADD(2, 0, 0x20),
			encADD(3, 0, 0x840),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstBSection, toOffset: 0x40},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups(
			[]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}},
			map[uint64]uint64{spConstASection: 0x301000, spConstBSection: 0x301800},
		)
		if err != nil {
			t.Fatal(err)
		}
		group := analysis.Groups[0]
		if group.Status != SplitPageCompatible {
			t.Fatalf("group status = %q (reason %q), want compatible", group.Status, group.Reason)
		}
		if group.NewTargetPage != 0x301000 {
			t.Fatalf("new target page = %#x, want 0x301000", group.NewTargetPage)
		}
		if group.NewADRPVmAddr != spTextAddr {
			t.Fatalf("new ADRP address = %#x, want %#x", group.NewADRPVmAddr, spTextAddr)
		}
		for _, consumer := range group.Consumers {
			spRequireStatus(t, consumer, SplitPageCompatible)
		}
	})

	t.Run("inconsistent_immediate_is_reported", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encADD(2, 0, 0x20), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x30}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := spFindRef(t, analysis, 4)
		spRequireStatus(t, ref, SplitPageInconsistent)
		if !strings.Contains(ref.Reason, "metadata declares") {
			t.Fatalf("reason = %q, want the declared target in the explanation", ref.Reason)
		}
		if len(analysis.Groups) != 1 || analysis.Groups[0].Status != SplitPageInconsistent {
			t.Fatalf("groups = %+v, want one inconsistent group", analysis.Groups)
		}
	})

	t.Run("barrier_marks_unknown", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			0x00000000, // UDF: an unsupported instruction is an analysis barrier
			encADD(2, 0, 0x20),
			encRET(),
		}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := spFindRef(t, analysis, 8)
		spRequireStatus(t, ref, SplitPageUnknown)
		if !strings.Contains(ref.Reason, "unreachable") {
			t.Fatalf("reason = %q, want a barrier explanation", ref.Reason)
		}
	})

	t.Run("unreachable_marks_unknown", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encB(pc, pc+8),
			encADD(2, 0, 0x20),
			encRET(),
		}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		spRequireStatus(t, spFindRef(t, analysis, 4), SplitPageUnknown)
	})

	t.Run("outside_ranges_marks_unknown", func(t *testing.T) {
		code := []uint32{encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 4}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := spFindRef(t, analysis, 8)
		spRequireStatus(t, ref, SplitPageUnknown)
		if !strings.Contains(ref.Reason, "outside") {
			t.Fatalf("reason = %q, want an outside-ranges explanation", ref.Reason)
		}
	})

	t.Run("str_preserves_base_register", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encSTRX(1, 0, 0x20),
			encADD(2, 0, 0x24),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x24},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		group := spFindGroup(t, analysis, 0)
		if len(group.Consumers) != 2 {
			t.Fatalf("consumers = %d, want the store and the following ADD", len(group.Consumers))
		}
		for _, consumer := range group.Consumers {
			spRequireStatus(t, consumer, SplitPageCompatible)
		}
	})

	t.Run("ldr_overwrites_base_register", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encLDRX(0, 0, 0x20),
			encADD(2, 0, 0x24),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x24},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		group := spFindGroup(t, analysis, 0)
		if len(group.Consumers) != 1 || group.Consumers[0].SourceOffset != 4 {
			t.Fatalf("consumers = %+v, want only the LDR at 4", group.Consumers)
		}
		spRequireStatus(t, group.Consumers[0], SplitPageCompatible)
		spRequireStatus(t, spFindRef(t, analysis, 8), SplitPageUnknown)
	})

	t.Run("zerofill_target_allowed", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spZerofillAddr), encADD(2, 0, 0x40), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spZerofillSection, toOffset: 0x40}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := spFindRef(t, analysis, 4)
		if ref.TargetSectionIndex != spZerofillSection || ref.TargetVmAddr != spZerofillAddr+0x40 {
			t.Fatalf("target = section %d %#x, want the zerofill section at %#x", ref.TargetSectionIndex, ref.TargetVmAddr, spZerofillAddr+0x40)
		}
		spRequireStatus(t, ref, SplitPageCompatible)
	})

	t.Run("zerofill_target_past_size_rejected", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spZerofillAddr), encADD(2, 0, 0x40), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spZerofillSection, toOffset: 0x1001}}
		f := newSplitPageFile(t, code, refs)
		_, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err == nil || !strings.Contains(err.Error(), "exceeds section") {
			t.Fatalf("error = %v, want an out-of-size target rejection", err)
		}
	})

	t.Run("duplicate_reference_merged", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encADD(2, 0, 0x20), encRET()}
		ref := spRef{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20}
		f := newSplitPageFile(t, code, []spRef{ref, ref})
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.References) != 1 {
			t.Fatalf("references = %d, want the duplicate merged", len(analysis.References))
		}
	})

	t.Run("conflicting_metadata_rejected", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encADD(2, 0, 0x20), encRET()}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x24},
		}
		f := newSplitPageFile(t, code, refs)
		_, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err == nil || !strings.Contains(err.Error(), "conflicting") {
			t.Fatalf("error = %v, want a conflicting-metadata rejection", err)
		}
	})

	t.Run("code_range_length_must_be_multiple_of_four", func(t *testing.T) {
		f := newSplitPageFile(t, []uint32{encRET()}, nil)
		_, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 6}}, nil)
		if err == nil || !strings.Contains(err.Error(), "multiple of four") {
			t.Fatalf("error = %v, want a length rejection", err)
		}
	})

	t.Run("code_ranges_must_not_overlap", func(t *testing.T) {
		f := newSplitPageFile(t, []uint32{encRET(), encRET()}, nil)
		_, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{
			{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 8},
			{SectionIndex: spTextSection, StartOffset: 4, EndOffset: 12},
		}, nil)
		if err == nil || !strings.Contains(err.Error(), "overlap") {
			t.Fatalf("error = %v, want an overlap rejection", err)
		}
	})

	t.Run("code_range_in_zerofill_rejected", func(t *testing.T) {
		f := newSplitPageFile(t, []uint32{encRET()}, nil)
		_, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spZerofillSection, StartOffset: 0, EndOffset: 4}}, nil)
		if err == nil || !strings.Contains(err.Error(), "no file-backed code") {
			t.Fatalf("error = %v, want a file-backing rejection", err)
		}
	})

	t.Run("section_zero_source_rejected", func(t *testing.T) {
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: 0, fromOffset: 0, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, []uint32{encRET()}, refs)
		_, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 4}}, nil)
		if err == nil || !strings.Contains(err.Error(), "image header") {
			t.Fatalf("error = %v, want a section-zero source rejection", err)
		}
	})

	t.Run("repeat_calls_are_deterministic", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),
			encADD(2, 0, 0x20),
			encADD(3, 0, 0x840),
			encRET(),
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 8, toSection: spConstBSection, toOffset: 0x40},
		}
		f := newSplitPageFile(t, code, refs)
		ranges := []SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}}
		bases := map[uint64]uint64{spConstASection: 0x301000, spConstBSection: 0x301800}
		first, err := f.AnalyzeSplitPageGroups(ranges, bases)
		if err != nil {
			t.Fatal(err)
		}
		second, err := f.AnalyzeSplitPageGroups(ranges, bases)
		if err != nil {
			t.Fatal(err)
		}
		if !reflect.DeepEqual(first, second) {
			t.Fatal("repeated analysis differs; report must not accumulate state")
		}
	})

	t.Run("interleaved_registers_mov_and_loop", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{
			encADRP(0, pc, spConstAAddr),   // 0x00
			encADRP(1, pc+4, spConstBAddr), // 0x04
			encMOV(3, 0),                   // 0x08: r3 copies r0's ADRP
			encMOV(4, 1),                   // 0x0c: r4 copies r1's ADRP
			encSTRX(9, 3, 0x20),            // 0x10: store through r3 (does not clobber r3)
			encMOV(3, 4),                   // 0x14: r3 now overwritten with r1's ADRP
			encLDRX(5, 3, 0x840),           // 0x18: load through r3 (clobbers r5, not r3)
			encCBNZ64(7, pc+0x1c, pc),      // 0x1c: loop back over both ADRPs
			encADD(6, 4, 0x824),            // 0x20: ADD through r4
			encRET(),                       // 0x24
		}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0x10, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0x18, toSection: spConstBSection, toOffset: 0x40},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 0x20, toSection: spConstBSection, toOffset: 0x24},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x28}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.Groups) != 2 {
			t.Fatalf("groups = %d, want one per ADRP", len(analysis.Groups))
		}
		// The store at 0x10 runs after the r3 <- r0 copy on both the first pass
		// and the loop backedge, so it belongs to the first ADRP.
		spRequireStatus(t, spFindRef(t, analysis, 0x10), SplitPageCompatible)
		r0Group := spFindGroup(t, analysis, 0)
		if len(r0Group.Consumers) != 1 || r0Group.Consumers[0].SourceOffset != 0x10 {
			t.Fatalf("r0 group consumers = %+v, want only the store at 0x10", r0Group.Consumers)
		}
		// The load at 0x18 and ADD at 0x20 follow the r3 <- r1 copy, so they
		// attach to the second ADRP and both target the same old page.
		r1Group := spFindGroup(t, analysis, 4)
		if len(r1Group.Consumers) != 2 {
			t.Fatalf("r1 group consumers = %d, want the load and the ADD", len(r1Group.Consumers))
		}
		for _, consumer := range r1Group.Consumers {
			spRequireStatus(t, consumer, SplitPageCompatible)
		}
	})

	t.Run("page_distance_overflow_is_conflict", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encADD(2, 0, 0x20), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		// Move the target 2^21 pages away from the ADRP: one page too far to
		// encode in ADRP's signed 21-bit displacement.
		tooFar := spConstAAddr + uint64(1<<21)<<12
		analysis, err := f.AnalyzeSplitPageGroups(
			[]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}},
			map[uint64]uint64{spConstASection: tooFar},
		)
		if err != nil {
			t.Fatal(err)
		}
		group := analysis.Groups[0]
		if group.Status != SplitPageConflict || !strings.Contains(group.Reason, "21-bit") {
			t.Fatalf("group = %q (%q), want an encodability conflict", group.Status, group.Reason)
		}
		if analysis.References[0].Status != SplitPageConflict {
			t.Fatalf("consumer status = %q, want conflict", analysis.References[0].Status)
		}
	})

	t.Run("non_pairing_references_ignored", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, spConstAAddr), encADD(2, 0, 0x20), encRET()}
		refs := []spRef{
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20},
			{kind: types.DYLD_CACHE_ADJ_V2_ARM64_BR26, fromSection: spTextSection, fromOffset: 8, toSection: spConstASection, toOffset: 0x24},
			{kind: types.DYLD_CACHE_ADJ_V2_POINTER_64, fromSection: spTextSection, fromOffset: 0xc, toSection: spConstASection, toOffset: 0x28},
		}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0x10}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		if len(analysis.References) != 1 {
			t.Fatalf("references = %d, want only the one OFF12", len(analysis.References))
		}
		if len(analysis.Groups) != 1 || analysis.Groups[0].Status != SplitPageCompatible {
			t.Fatalf("groups = %+v, want a single compatible group", analysis.Groups)
		}
	})

	t.Run("image_header_target_uses_base_address", func(t *testing.T) {
		pc := spTextAddr
		code := []uint32{encADRP(0, pc, 0x100000), encADD(2, 0, 0x20), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: 0, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		headerBase := f.GetBaseAddress()
		if headerBase != 0x100000 {
			t.Fatalf("base address = %#x, want 0x100000", headerBase)
		}
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := spFindRef(t, analysis, 4)
		if ref.TargetSectionIndex != 0 || ref.TargetVmAddr != headerBase+0x20 {
			t.Fatalf("target = section %d %#x, want image header at %#x", ref.TargetSectionIndex, ref.TargetVmAddr, headerBase+0x20)
		}
		spRequireStatus(t, ref, SplitPageCompatible)
		if ref.NewTargetVmAddr != ref.TargetVmAddr {
			t.Fatalf("new target = %#x, want the old base to be retained", ref.NewTargetVmAddr)
		}
	})

	t.Run("unproven_paths_keep_reason", func(t *testing.T) {
		// r0 is loaded from memory, so its provenance cannot be an ADRP.
		code := []uint32{encLDRX(0, 9, 0x10), encADD(2, 0, 0x20), encRET()}
		refs := []spRef{{kind: types.DYLD_CACHE_ADJ_V2_ARM64_OFF12, fromSection: spTextSection, fromOffset: 4, toSection: spConstASection, toOffset: 0x20}}
		f := newSplitPageFile(t, code, refs)
		analysis, err := f.AnalyzeSplitPageGroups([]SplitPageCodeRange{{SectionIndex: spTextSection, StartOffset: 0, EndOffset: 0xc}}, nil)
		if err != nil {
			t.Fatal(err)
		}
		ref := spFindRef(t, analysis, 4)
		spRequireStatus(t, ref, SplitPageUnknown)
		if ref.Reason == "" {
			t.Fatal("unknown status must retain a reason")
		}
		if len(analysis.Groups) != 0 {
			t.Fatalf("groups = %d, want none", len(analysis.Groups))
		}
	})
}
