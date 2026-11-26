package macho

import (
	"fmt"
	"sort"

	"github.com/blacktop/go-macho/types"
)

// SplitPageStatus is the verdict for one ARM64_OFF12 reference or for the
// group of consumers that share one ADRP instruction.
type SplitPageStatus string

const (
	// SplitPageCompatible means every consistent consumer in the group
	// resolves to the same new target page and the page distance still fits
	// in ADRP's signed 21-bit displacement.
	SplitPageCompatible SplitPageStatus = "compatible"
	// SplitPageConflict means the consumers in one ADRP group no longer share
	// one page in the new layout, or the new page distance cannot be encoded.
	SplitPageConflict SplitPageStatus = "conflict"
	// SplitPageInconsistent means the instruction bytes do not reproduce the
	// target that LC_SEGMENT_SPLIT_INFO declares for the reference.
	SplitPageInconsistent SplitPageStatus = "inconsistent"
	// SplitPageAmbiguous means data flow reached the consumer with more than
	// one possible ADRP source, or with a mix of an ADRP and a non-ADRP path.
	SplitPageAmbiguous SplitPageStatus = "ambiguous"
	// SplitPageUnknown means the analysis could not prove an ADRP provenance,
	// either because the consumer is unreachable or because an analysis
	// barrier stopped the path.
	SplitPageUnknown SplitPageStatus = "unknown"
)

// SplitPageCodeRange describes a half-open range of ARM64 instructions inside
// one section. SectionIndex uses the same one-based numbering as
// LC_SEGMENT_SPLIT_INFO: index 0 is the Mach-O header and the first section is
// index 1. StartOffset and EndOffset are byte offsets from the section's start;
// EndOffset-StartOffset must be a multiple of four. Ranges must not overlap
// and must lie in file-backed (that is, not zero-fill) section data.
type SplitPageCodeRange struct {
	SectionIndex uint64
	StartOffset  uint64
	EndOffset    uint64
}

// SplitPageReference is one LC_SEGMENT_SPLIT_INFO ARM64_OFF12 reference and
// the result of analyzing its instruction. SourceVmAddr and TargetVmAddr are
// the addresses declared by the metadata. NewSourceVmAddr and NewTargetVmAddr
// are the addresses computed with the caller-supplied section bases; sections
// without an explicit new base keep their old base.
type SplitPageReference struct {
	SourceSectionIndex uint64
	SourceOffset       uint64
	TargetSectionIndex uint64
	TargetOffset       uint64
	SourceVmAddr       uint64
	NewSourceVmAddr    uint64
	TargetVmAddr       uint64
	NewTargetVmAddr    uint64
	Status             SplitPageStatus
	Reason             string
}

// SplitPageGroup contains the ARM64_OFF12 consumers whose base-register data
// flow proves they share one ADRP instruction. ADRPVmAddr is the address of
// that ADRP instruction and NewADRPVmAddr is its address under the requested
// layout.
type SplitPageGroup struct {
	ADRPSectionIndex  uint64
	ADRPSectionOffset uint64
	ADRPVmAddr        uint64
	NewADRPVmAddr     uint64
	ADRPTargetPage    uint64
	NewTargetPage     uint64
	Status            SplitPageStatus
	Reason            string
	Consumers         []*SplitPageReference
}

// SplitPageAnalysis is the read-only result of File.AnalyzeSplitPageGroups.
// References contains every ARM64_OFF12 reference from the split-info payload,
// while Groups contains only consumers with a uniquely proven ADRP source.
// Both slices are sorted deterministically.
type SplitPageAnalysis struct {
	Groups     []*SplitPageGroup
	References []*SplitPageReference
}

type splitPageProvKind uint8

const (
	splitPageProvNone splitPageProvKind = iota
	splitPageProvADRP
	splitPageProvAmbiguous
)

type splitPageReg struct {
	kind splitPageProvKind
	addr uint64
}

type splitPageState struct {
	reached bool
	regs    [32]splitPageReg
}

type splitPageInstruction struct {
	addr         uint64
	sectionIndex uint64
	offset       uint64
	insn         uint32
}

type splitPageRangePlan struct {
	sectionIndex uint64
	start        uint64
	end          uint64
	startAddr    uint64
}

type splitPageRefKey struct {
	kind        types.SplitInfoKind
	fromSection uint64
	fromOffset  uint64
}

type splitPageRawRef struct {
	toSection uint64
	toOffset  uint64
}

type splitPageConsumer struct {
	report        *SplitPageReference
	consistent    bool
	overflow      bool
	newTargetPage uint64
}

// AnalyzeSplitPageGroups pairs ARM64_OFF12 references with the ADRP instruction
// that actually supplies their base register, then checks whether each such
// group survives a proposed image layout. It reads the real
// LC_SEGMENT_SPLIT_INFO payload, never rewrites instructions, never calls
// Export and never executes the image.
//
// newBases optionally maps a one-based split-info section index to the virtual
// base that section would have in the new layout. Index 0 addresses the
// Mach-O header, whose old base is GetBaseAddress. Sections without an entry
// keep their old base.
func (f *File) AnalyzeSplitPageGroups(codeRanges []SplitPageCodeRange, newBases map[uint64]uint64) (*SplitPageAnalysis, error) {
	if f == nil {
		return nil, fmt.Errorf("cannot analyze split page groups of a nil file")
	}
	if !f.isArm64() {
		return nil, fmt.Errorf("split page group analysis requires an arm64 Mach-O file")
	}
	sections := f.Sections
	for index := range newBases {
		if index > uint64(len(sections)) {
			return nil, fmt.Errorf("new base for section index %d is out of range", index)
		}
	}

	oldBase := func(index uint64) (uint64, error) {
		if index == 0 {
			return f.GetBaseAddress(), nil
		}
		if index > uint64(len(sections)) {
			return 0, fmt.Errorf("section index %d is out of range", index)
		}
		return sections[index-1].Addr, nil
	}
	newBase := func(index uint64) (uint64, error) {
		base, err := oldBase(index)
		if err != nil {
			return 0, err
		}
		if next, ok := newBases[index]; ok {
			return next, nil
		}
		return base, nil
	}

	plans, err := f.splitPageValidateRanges(codeRanges)
	if err != nil {
		return nil, err
	}
	insns, addrToIndex, entries, err := f.splitPageReadRanges(plans)
	if err != nil {
		return nil, err
	}
	refs, err := f.splitPageReadReferences()
	if err != nil {
		return nil, err
	}

	states := splitPageSolveDataFlow(insns, addrToIndex, entries)

	reports := make([]*SplitPageReference, 0, len(refs.order))
	consumersByADRP := make(map[uint64][]*splitPageConsumer)
	adrpOrder := make([]uint64, 0)

	for _, key := range refs.order {
		if key.kind != types.DYLD_CACHE_ADJ_V2_ARM64_OFF12 {
			continue
		}
		raw := refs.byKey[key]

		srcOldBase, err := oldBase(key.fromSection)
		if err != nil {
			return nil, err
		}
		srcNewBase, err := newBase(key.fromSection)
		if err != nil {
			return nil, err
		}
		tgtOldBase, err := oldBase(raw.toSection)
		if err != nil {
			return nil, err
		}
		tgtNewBase, err := newBase(raw.toSection)
		if err != nil {
			return nil, err
		}
		sourceVmAddr, err := splitPageAddAddress(srcOldBase, key.fromOffset)
		if err != nil {
			return nil, err
		}
		newSourceVmAddr, err := splitPageAddAddress(srcNewBase, key.fromOffset)
		if err != nil {
			return nil, err
		}
		targetVmAddr, err := splitPageAddAddress(tgtOldBase, raw.toOffset)
		if err != nil {
			return nil, err
		}
		newTargetVmAddr, err := splitPageAddAddress(tgtNewBase, raw.toOffset)
		if err != nil {
			return nil, err
		}

		report := &SplitPageReference{
			SourceSectionIndex: key.fromSection,
			SourceOffset:       key.fromOffset,
			TargetSectionIndex: raw.toSection,
			TargetOffset:       raw.toOffset,
			SourceVmAddr:       sourceVmAddr,
			NewSourceVmAddr:    newSourceVmAddr,
			TargetVmAddr:       targetVmAddr,
			NewTargetVmAddr:    newTargetVmAddr,
			Status:             SplitPageUnknown,
		}
		reports = append(reports, report)

		nodeIndex, ok := addrToIndex[sourceVmAddr]
		if !ok {
			report.Reason = "source is outside the analyzed code ranges"
			continue
		}
		node := &insns[nodeIndex]
		if !states[nodeIndex].reached {
			report.Reason = "consumer is unreachable from any code range entry (analysis barrier)"
			continue
		}
		baseRegister, ok := splitPageConsumerBase(node.insn)
		if !ok {
			report.Reason = "source instruction is not a supported ADD or unsigned-offset LDR/STR"
			continue
		}
		prov := splitPageReadReg(states[nodeIndex], baseRegister)
		switch prov.kind {
		case splitPageProvAmbiguous:
			report.Status = SplitPageAmbiguous
			report.Reason = "base register has ambiguous ADRP provenance (multiple ADRPs or a non-ADRP path)"
			continue
		case splitPageProvNone:
			report.Reason = "base register has no ADRP provenance"
			continue
		}

		adrpIndex, ok := addrToIndex[prov.addr]
		if !ok {
			report.Reason = "ADRP provenance is outside the analyzed code ranges"
			continue
		}
		adrpNode := &insns[adrpIndex]
		byteOffset, ok := splitPageDecodeOff12(node.insn)
		if !ok {
			report.Reason = "source instruction has no decodable page offset"
			continue
		}

		adrpPage := splitPageADRPTargetPage(adrpNode.insn, adrpNode.addr)
		consumer := &splitPageConsumer{
			report:        report,
			newTargetPage: newTargetVmAddr &^ uint64(0xFFF),
		}
		if expected := adrpPage + byteOffset; expected != targetVmAddr {
			report.Status = SplitPageInconsistent
			report.Reason = fmt.Sprintf(
				"encoded ADRP page %#x plus instruction offset %#x is %#x, metadata declares %#x",
				adrpPage, byteOffset, expected, targetVmAddr)
		} else {
			consumer.consistent = true
		}
		if _, seen := consumersByADRP[prov.addr]; !seen {
			adrpOrder = append(adrpOrder, prov.addr)
		}
		consumersByADRP[prov.addr] = append(consumersByADRP[prov.addr], consumer)
	}

	groups := make([]*SplitPageGroup, 0, len(adrpOrder))
	for _, adrpAddr := range adrpOrder {
		consumers := consumersByADRP[adrpAddr]
		adrpNode := &insns[addrToIndex[adrpAddr]]
		adrpNewBase, err := newBase(adrpNode.sectionIndex)
		if err != nil {
			return nil, err
		}
		newADRPAddr, err := splitPageAddAddress(adrpNewBase, adrpNode.offset)
		if err != nil {
			return nil, err
		}
		group := &SplitPageGroup{
			ADRPSectionIndex:  adrpNode.sectionIndex,
			ADRPSectionOffset: adrpNode.offset,
			ADRPVmAddr:        adrpNode.addr,
			NewADRPVmAddr:     newADRPAddr,
			ADRPTargetPage:    splitPageADRPTargetPage(adrpNode.insn, adrpNode.addr),
		}

		// Only consumers whose instruction bytes reproduce the metadata can
		// prove an ADRP group; an inconsistent consumer is reported on its own.
		pages := make(map[uint64]struct{})
		overflow := false
		consistentCount := 0
		newADRPPage := newADRPAddr &^ uint64(0xFFF)
		for _, consumer := range consumers {
			if !consumer.consistent {
				continue
			}
			consistentCount++
			pages[consumer.newTargetPage] = struct{}{}
			delta := int64(consumer.newTargetPage>>12) - int64(newADRPPage>>12)
			if delta < -(1<<20) || delta > (1<<20)-1 {
				consumer.overflow = true
				overflow = true
			}
		}

		switch {
		case consistentCount == 0:
			group.Status = SplitPageInconsistent
			group.Reason = "no consumer could be verified against the split-info metadata"
		case len(pages) > 1:
			group.Status = SplitPageConflict
			group.Reason = fmt.Sprintf("consumers resolve to %d different target pages in the new layout", len(pages))
		case overflow:
			group.Status = SplitPageConflict
			group.Reason = "new target page distance does not fit in ADRP's signed 21-bit displacement"
		default:
			group.Status = SplitPageCompatible
			for page := range pages {
				group.NewTargetPage = page
			}
		}

		for _, consumer := range consumers {
			if consumer.consistent {
				switch group.Status {
				case SplitPageConflict:
					consumer.report.Status = SplitPageConflict
					switch {
					case consumer.overflow:
						consumer.report.Reason = "new target page distance does not fit in ADRP's signed 21-bit displacement"
					case len(pages) > 1:
						consumer.report.Reason = fmt.Sprintf(
							"consumer targets page %#x, but its ADRP group spans %d pages",
							consumer.newTargetPage, len(pages))
					default:
						consumer.report.Reason = group.Reason
					}
				case SplitPageCompatible:
					consumer.report.Status = SplitPageCompatible
					consumer.report.Reason = ""
				}
			} else {
				consumer.report.Status = SplitPageInconsistent
			}
			group.Consumers = append(group.Consumers, consumer.report)
		}
		sort.SliceStable(group.Consumers, func(i, j int) bool {
			return splitPageReferenceLess(group.Consumers[i], group.Consumers[j])
		})
		groups = append(groups, group)
	}

	sort.SliceStable(reports, func(i, j int) bool {
		return splitPageReferenceLess(reports[i], reports[j])
	})
	sort.SliceStable(groups, func(i, j int) bool {
		a, b := groups[i], groups[j]
		if a.ADRPSectionIndex != b.ADRPSectionIndex {
			return a.ADRPSectionIndex < b.ADRPSectionIndex
		}
		if a.ADRPSectionOffset != b.ADRPSectionOffset {
			return a.ADRPSectionOffset < b.ADRPSectionOffset
		}
		return a.ADRPVmAddr < b.ADRPVmAddr
	})

	return &SplitPageAnalysis{Groups: groups, References: reports}, nil
}

func splitPageReferenceLess(a, b *SplitPageReference) bool {
	if a.SourceSectionIndex != b.SourceSectionIndex {
		return a.SourceSectionIndex < b.SourceSectionIndex
	}
	if a.SourceOffset != b.SourceOffset {
		return a.SourceOffset < b.SourceOffset
	}
	if a.TargetSectionIndex != b.TargetSectionIndex {
		return a.TargetSectionIndex < b.TargetSectionIndex
	}
	return a.TargetOffset < b.TargetOffset
}

func splitPageAddAddress(base, offset uint64) (uint64, error) {
	if base > ^uint64(0)-offset {
		return 0, fmt.Errorf("address overflows: base %#x offset %#x", base, offset)
	}
	return base + offset, nil
}

func splitPageIsZerofill(sec *types.Section) bool {
	return sec.Flags.IsZerofill() || sec.Flags.IsGbZerofill() || sec.Flags.IsThreadLocalZerofill()
}

func (f *File) splitPageValidateRanges(codeRanges []SplitPageCodeRange) ([]splitPageRangePlan, error) {
	sections := f.Sections
	plans := make([]splitPageRangePlan, 0, len(codeRanges))
	for i, r := range codeRanges {
		if r.SectionIndex == 0 {
			return nil, fmt.Errorf("code range %d: section index 0 is the image header and cannot be a code source", i)
		}
		if r.SectionIndex > uint64(len(sections)) {
			return nil, fmt.Errorf("code range %d: section index %d is out of range", i, r.SectionIndex)
		}
		sec := sections[r.SectionIndex-1]
		if r.EndOffset < r.StartOffset {
			return nil, fmt.Errorf("code range %d: end offset %#x precedes start offset %#x", i, r.EndOffset, r.StartOffset)
		}
		length := r.EndOffset - r.StartOffset
		if length%4 != 0 {
			return nil, fmt.Errorf("code range %d: length %#x is not a multiple of four", i, length)
		}
		if r.EndOffset > sec.Size {
			return nil, fmt.Errorf("code range %d: end offset %#x exceeds section %d size %#x", i, r.EndOffset, r.SectionIndex, sec.Size)
		}
		if length == 0 {
			continue
		}
		if splitPageIsZerofill(sec) {
			return nil, fmt.Errorf("code range %d: section %d has no file-backed code", i, r.SectionIndex)
		}
		startAddr, err := splitPageAddAddress(sec.Addr, r.StartOffset)
		if err != nil {
			return nil, err
		}
		if startAddr%4 != 0 {
			return nil, fmt.Errorf("code range %d: start address %#x is not four-byte aligned", i, startAddr)
		}
		if !f.hasFileData(startAddr, length) {
			return nil, fmt.Errorf("code range %d: [%#x,%#x) is not backed by file data", i, r.StartOffset, r.EndOffset)
		}
		plans = append(plans, splitPageRangePlan{
			sectionIndex: r.SectionIndex,
			start:        r.StartOffset,
			end:          r.EndOffset,
			startAddr:    startAddr,
		})
	}

	for i := 0; i < len(plans); i++ {
		for j := i + 1; j < len(plans); j++ {
			a, b := plans[i], plans[j]
			aEnd := a.startAddr + (a.end - a.start)
			bEnd := b.startAddr + (b.end - b.start)
			if a.startAddr < bEnd && b.startAddr < aEnd {
				return nil, fmt.Errorf("code ranges [%d:%#x,%#x) and [%d:%#x,%#x) overlap",
					a.sectionIndex, a.start, a.end, b.sectionIndex, b.start, b.end)
			}
		}
	}
	return plans, nil
}

func (f *File) splitPageReadRanges(plans []splitPageRangePlan) ([]splitPageInstruction, map[uint64]int, []int, error) {
	var insns []splitPageInstruction
	addrToIndex := make(map[uint64]int)
	var entries []int
	for _, plan := range plans {
		length := plan.end - plan.start
		buf := make([]byte, length)
		n, err := f.cr.ReadAtAddr(buf, plan.startAddr)
		if n != int(length) {
			if err == nil {
				err = fmt.Errorf("short read")
			}
			return nil, nil, nil, fmt.Errorf("failed to read code range %d:%#x: %v", plan.sectionIndex, plan.start, err)
		}
		first := len(insns)
		for off := uint64(0); off < length; off += 4 {
			addr := plan.startAddr + off
			if _, ok := addrToIndex[addr]; ok {
				return nil, nil, nil, fmt.Errorf("code ranges overlap at address %#x", addr)
			}
			addrToIndex[addr] = len(insns)
			insns = append(insns, splitPageInstruction{
				addr:         addr,
				sectionIndex: plan.sectionIndex,
				offset:       plan.start + off,
				insn:         f.ByteOrder.Uint32(buf[off:]),
			})
		}
		entries = append(entries, first)
	}
	return insns, addrToIndex, entries, nil
}

type splitPageRefs struct {
	order []splitPageRefKey
	byKey map[splitPageRefKey]splitPageRawRef
}

// splitPageReadReferences decodes ARM64_ADRP and ARM64_OFF12 references from
// the real LC_SEGMENT_SPLIT_INFO payload, merges identical entries, and rejects
// metadata that describes one instruction twice with different targets.
func (f *File) splitPageReadReferences() (*splitPageRefs, error) {
	sections := f.Sections
	out := &splitPageRefs{byKey: make(map[splitPageRefKey]splitPageRawRef)}
	var metadataErr error
	err := f.ForEachV2SplitSegReference(func(fromSection, fromOffset, toSection, toOffset uint64, kind types.SplitInfoKind) {
		if metadataErr != nil {
			return
		}
		if kind != types.DYLD_CACHE_ADJ_V2_ARM64_ADRP && kind != types.DYLD_CACHE_ADJ_V2_ARM64_OFF12 {
			return
		}
		if fromSection == 0 {
			metadataErr = fmt.Errorf("%s reference at section 0 offset %#x has the image header as its source", kind, fromOffset)
			return
		}
		if fromSection > uint64(len(sections)) {
			metadataErr = fmt.Errorf("%s reference source section index %d is out of range", kind, fromSection)
			return
		}
		if toSection > uint64(len(sections)) {
			metadataErr = fmt.Errorf("%s reference target section index %d is out of range", kind, toSection)
			return
		}
		if toSection != 0 && toOffset > sections[toSection-1].Size {
			metadataErr = fmt.Errorf("%s reference target offset %#x exceeds section %d size %#x",
				kind, toOffset, toSection, sections[toSection-1].Size)
			return
		}
		from := sections[fromSection-1]
		if splitPageIsZerofill(from) {
			metadataErr = fmt.Errorf("%s reference source section %d has no file-backed bytes", kind, fromSection)
			return
		}
		if fromOffset > from.Size || from.Size-fromOffset < 4 {
			metadataErr = fmt.Errorf("%s reference source offset %#x is outside section %d size %#x",
				kind, fromOffset, fromSection, from.Size)
			return
		}
		fromAddr, err := splitPageAddAddress(from.Addr, fromOffset)
		if err != nil {
			metadataErr = err
			return
		}
		if !f.hasFileData(fromAddr, 4) {
			metadataErr = fmt.Errorf("%s reference source %d:%#x is not backed by file data", kind, fromSection, fromOffset)
			return
		}
		key := splitPageRefKey{kind: kind, fromSection: fromSection, fromOffset: fromOffset}
		if previous, ok := out.byKey[key]; ok {
			if previous.toSection != toSection || previous.toOffset != toOffset {
				metadataErr = fmt.Errorf("conflicting LC_SEGMENT_SPLIT_INFO references for %s at section %d offset %#x",
					kind, fromSection, fromOffset)
			}
			return
		}
		out.byKey[key] = splitPageRawRef{toSection: toSection, toOffset: toOffset}
		out.order = append(out.order, key)
	})
	if err != nil {
		return nil, err
	}
	if metadataErr != nil {
		return nil, metadataErr
	}
	return out, nil
}

func splitPageSolveDataFlow(insns []splitPageInstruction, addrToIndex map[uint64]int, entries []int) []splitPageState {
	states := make([]splitPageState, len(insns))
	succ := make([][]int, len(insns))
	for i := range insns {
		succ[i] = splitPageSuccessors(insns[i].insn, insns[i].addr, addrToIndex)
	}
	queue := make([]int, 0, len(insns))
	for _, entry := range entries {
		if splitPageMergeState(&states[entry], splitPageState{reached: true}) {
			queue = append(queue, entry)
		}
	}
	for head := 0; head < len(queue); head++ {
		i := queue[head]
		out := splitPageTransfer(states[i], insns[i].insn, insns[i].addr)
		for _, next := range succ[i] {
			if splitPageMergeState(&states[next], out) {
				queue = append(queue, next)
			}
		}
	}
	return states
}

func splitPageMergeState(dst *splitPageState, src splitPageState) bool {
	if !src.reached {
		return false
	}
	if !dst.reached {
		*dst = src
		return true
	}
	changed := false
	for i := range dst.regs {
		merged := splitPageMergeReg(dst.regs[i], src.regs[i])
		if merged != dst.regs[i] {
			dst.regs[i] = merged
			changed = true
		}
	}
	return changed
}

func splitPageMergeReg(a, b splitPageReg) splitPageReg {
	if a == b {
		return a
	}
	if a.kind == splitPageProvAmbiguous || b.kind == splitPageProvAmbiguous {
		return splitPageReg{kind: splitPageProvAmbiguous}
	}
	if a.kind == splitPageProvADRP && b.kind == splitPageProvADRP && a.addr == b.addr {
		return a
	}
	return splitPageReg{kind: splitPageProvAmbiguous}
}

func splitPageReadReg(state splitPageState, reg uint32) splitPageReg {
	if reg == 31 {
		return splitPageReg{}
	}
	return state.regs[reg]
}

func splitPageTransfer(in splitPageState, insn uint32, pc uint64) splitPageState {
	out := in
	switch {
	case splitPageIsADRP(insn):
		if rd := insn & 0x1F; rd != 31 {
			out.regs[rd] = splitPageReg{kind: splitPageProvADRP, addr: pc}
		}
	case splitPageIsMOV64(insn):
		rd, rm := insn&0x1F, (insn>>16)&0x1F
		if rd != 31 {
			out.regs[rd] = splitPageReadReg(in, rm)
		}
	case splitPageIsADDImm(insn):
		if rd := insn & 0x1F; rd != 31 {
			out.regs[rd] = splitPageReg{}
		}
	default:
		if store, rt, ok := splitPageLoadStore(insn); ok && !store {
			if rt != 31 {
				out.regs[rt] = splitPageReg{}
			}
		}
	}
	return out
}

func splitPageSuccessors(insn uint32, pc uint64, addrToIndex map[uint64]int) []int {
	lookup := func(addr uint64) (int, bool) {
		i, ok := addrToIndex[addr]
		return i, ok
	}
	switch {
	case splitPageIsB(insn):
		if target, ok := lookup(splitPageBranchTarget(insn, pc)); ok {
			return []int{target}
		}
		return nil
	case splitPageIsBCond(insn), splitPageIsCBZ(insn):
		var out []int
		if target, ok := lookup(splitPageBranchTarget(insn, pc)); ok {
			out = append(out, target)
		}
		if next, ok := lookup(pc + 4); ok {
			out = append(out, next)
		}
		return out
	case splitPageIsRET(insn):
		return nil
	case splitPageIsNOP(insn),
		splitPageIsADRP(insn),
		splitPageIsMOV64(insn),
		splitPageIsADDImm(insn):
		if next, ok := lookup(pc + 4); ok {
			return []int{next}
		}
		return nil
	default:
		if _, _, ok := splitPageLoadStore(insn); ok {
			if next, ok := lookup(pc + 4); ok {
				return []int{next}
			}
			return nil
		}
		// Unknown instructions are analysis barriers: do not guess through them.
		return nil
	}
}

func splitPageConsumerBase(insn uint32) (uint32, bool) {
	if splitPageIsADDImm(insn) {
		return (insn >> 5) & 0x1F, true
	}
	if _, _, ok := splitPageLoadStore(insn); ok {
		return (insn >> 5) & 0x1F, true
	}
	return 0, false
}

func splitPageIsADRP(insn uint32) bool  { return insn&0x9F000000 == 0x90000000 }
func splitPageIsMOV64(insn uint32) bool { return insn&0xFFE0FFE0 == 0xAA0003E0 }

// splitPageIsADDImm matches the unshifted ADD (immediate) forms, both 32-bit
// (0x11) and 64-bit (0x91). The mask includes the sh bit, so shifted forms are
// not matched.
func splitPageIsADDImm(insn uint32) bool {
	switch insn & 0xFFC00000 {
	case 0x91000000, 0x11000000:
		return true
	}
	return false
}
func splitPageIsNOP(insn uint32) bool { return insn == 0xD503201F }
func splitPageIsRET(insn uint32) bool { return insn&0xFFFFFC1F == 0xD65F0000 }
func splitPageIsB(insn uint32) bool   { return insn&0xFC000000 == 0x14000000 }
func splitPageIsBCond(insn uint32) bool {
	return insn&0xFF000010 == 0x54000000
}
func splitPageIsCBZ(insn uint32) bool {
	masked := insn & 0x7F000000
	return masked == 0x34000000 || masked == 0x35000000
}

// splitPageDecodeOff12 returns the byte offset encoded by a supported ADD or
// LDR/STR, applying the access-size scaling of a W/X load or store.
func splitPageDecodeOff12(insn uint32) (uint64, bool) {
	if _, _, ok := splitPageLoadStore(insn); ok {
		imm := uint64((insn >> 10) & 0xFFF)
		if insn>>30 == 2 { // W register: scale by 4
			return imm * 4, true
		}
		return imm * 8, true // X register: scale by 8
	}
	if splitPageIsADDImm(insn) {
		return uint64((insn >> 10) & 0xFFF), true
	}
	return 0, false
}

// splitPageLoadStore reports whether insn is an unsigned-offset LDR/STR of a
// W or X general-purpose register, and whether it stores.
func splitPageLoadStore(insn uint32) (store bool, rt uint32, ok bool) {
	if insn&0x3B000000 != 0x39000000 {
		return false, 0, false
	}
	// Bit 26 is V: the SIMD&FP forms use different scaling and registers.
	if insn&0x04000000 != 0 {
		return false, 0, false
	}
	// Only W (size 10) and X (size 11) registers are in scope; byte and
	// halfword accesses and the sign-extending/prefetch forms are not.
	if size := insn >> 30; size != 2 && size != 3 {
		return false, 0, false
	}
	opc := (insn >> 22) & 0x3
	if opc > 1 {
		return false, 0, false
	}
	return opc == 0, insn & 0x1F, true
}

func splitPageBranchTarget(insn uint32, pc uint64) uint64 {
	if splitPageIsB(insn) {
		imm := splitPageSignExtend(uint64(insn&0x03FFFFFF), 26)
		return pc + (imm << 2)
	}
	imm := splitPageSignExtend(uint64((insn>>5)&0x7FFFF), 19)
	return pc + (imm << 2)
}

func splitPageSignExtend(value uint64, bits uint) uint64 {
	if value&(uint64(1)<<(bits-1)) != 0 {
		value |= ^uint64(0) << bits
	}
	return value
}

func splitPageADRPTargetPage(insn uint32, pc uint64) uint64 {
	imm := uint64((insn>>5)&0x7FFFF)<<2 | uint64((insn>>29)&0x3)
	imm = splitPageSignExtend(imm, 21)
	return (pc &^ uint64(0xFFF)) + (imm << 12)
}
