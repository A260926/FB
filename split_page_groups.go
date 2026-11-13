package macho

// Static analysis of LC_SEGMENT_SPLIT_INFO v2 ADRP/OFF12 references.
//
// AnalyzeSplitPageGroups answers two questions without touching the image:
// which ARM64_OFF12 references really share one ADRP (proved through the
// control flow instead of guessed from instruction proximity), and whether
// that sharing still encodes once sections move to hypothetical new virtual
// bases.

import (
	"encoding/binary"
	"fmt"
	"slices"

	"github.com/blacktop/go-macho/types"
)

// SplitPageCodeRange is a half-open code interval described with the same
// one-based section numbering as LC_SEGMENT_SPLIT_INFO: section zero is the
// image header and can never be a code source. Offset and Length are counted
// in bytes inside the section; the interval is [Offset, Offset+Length).
type SplitPageCodeRange struct {
	SectionIndex int
	Offset       uint64
	Length       uint64
}

// SplitPageGroupStatus is the final verdict for one OFF12 reference, or for
// the whole group of OFF12 references fed by one ADRP.
type SplitPageGroupStatus uint8

const (
	// SplitPageStatusCompatible means every consumer of the group still
	// addresses the ADRP's new target page and the 21-bit ADRP page distance
	// stays encodable.
	SplitPageStatusCompatible SplitPageGroupStatus = iota
	// SplitPageStatusConflict means consumers of one ADRP land on different
	// pages in the new layout, or a required signed 21-bit page distance no
	// longer fits. Every involved consumer is reported.
	SplitPageStatusConflict
	// SplitPageStatusAmbiguous means the consumer's base register is fed by
	// distinct ADRPs on different paths, or by at least one path that does not
	// carry an ADRP source at all.
	SplitPageStatusAmbiguous
	// SplitPageStatusUnknown means no single ADRP source can be proved,
	// either because the register is never established on a reachable path or
	// because every proving path crosses an analysis barrier. Reason explains
	// why.
	SplitPageStatusUnknown
	// SplitPageStatusInconsistent means the ADRP's signed page displacement
	// plus the consumer's encoded immediate and memory access scale do not
	// reconstruct the target declared by LC_SEGMENT_SPLIT_INFO.
	SplitPageStatusInconsistent
)

func (s SplitPageGroupStatus) String() string {
	switch s {
	case SplitPageStatusCompatible:
		return "compatible"
	case SplitPageStatusConflict:
		return "conflict"
	case SplitPageStatusAmbiguous:
		return "ambiguous"
	case SplitPageStatusUnknown:
		return "unknown"
	case SplitPageStatusInconsistent:
		return "inconsistent"
	default:
		return fmt.Sprintf("invalid status %d", uint8(s))
	}
}

// SplitPageReference is one analyzed ARM64_OFF12 reference. Section indices
// use the LC_SEGMENT_SPLIT_INFO convention where zero names the image header.
type SplitPageReference struct {
	Kind                types.SplitInfoKind
	SourceSectionIndex  int
	SourceSectionOffset uint64
	TargetSectionIndex  int
	TargetSectionOffset uint64
	SourceVmAddr        uint64 // original VM address of the OFF12 instruction
	TargetVmAddr        uint64 // original VM address declared by the metadata
	NewSourceVmAddr     uint64 // hypothetical VM address after remapping
	NewTargetVmAddr     uint64 // hypothetical VM address after remapping
	Status              SplitPageGroupStatus
	// Reason explains every verdict except SplitPageStatusCompatible. It
	// names the barrier or unprovable register for unknown consumers, the
	// clashing provenance for ambiguous ones, the broken encoding for
	// inconsistent ones, and the page split or overflow for conflicts.
	Reason string
	// Group is the ADRP group this reference was proved to belong to. It is
	// nil for ambiguous and unknown references, which have no unique ADRP.
	Group *SplitPageGroup
}

// SplitPageGroup collects every OFF12 reference proved to consume the same
// ADRP instruction.
type SplitPageGroup struct {
	ADRPSectionIndex  int
	ADRPSectionOffset uint64
	ADRPVmAddr        uint64 // original VM address of the ADRP instruction
	NewADRPVmAddr     uint64 // hypothetical VM address after remapping
	Status            SplitPageGroupStatus
	References        []*SplitPageReference
}

// SplitPageReport is the read-only result of AnalyzeSplitPageGroups. Both
// slices are sorted deterministically and building the report leaves no state
// on the File, so repeated calls return equal results.
type SplitPageReport struct {
	References []*SplitPageReference
	Groups     []*SplitPageGroup
}

// spgProv is the per-register provenance lattice used by the dataflow.
type spgProv uint8

const (
	provUnknown   spgProv = iota // bottom: nothing provable about the register
	provADRP                     // holds the page address of one ADRP node
	provOther                    // provably holds a value that is not an ADRP page
	provBarrier                  // unprovable because a path crossed a barrier
	provAmbiguous                // top: conflicting proven sources were merged
)

type spgReg struct {
	prov spgProv
	adrp *spgNode // valid only when prov == provADRP
}

type spgEdge struct {
	to      *spgNode
	barrier bool // the edge carries all-barrier state instead of the predecessor's
}

type spgNode struct {
	sect int
	off  uint64
	vm   uint64
	insn uint32

	edges []spgEdge // successors, including the fall-through edge

	in  []*spgNode // predecessors, for fixed-point iteration
	out [32]spgReg // outgoing register state once converged

	consumer bool // an ARM64_OFF12 metadata source
	base     uint32
	result   spgReg // provenance of the base register at this consumer
	reason   string // explanation when result is not a unique ADRP
	adrp     *spgNode
}

type spgMetaRef struct {
	kind              types.SplitInfoKind
	fromSect, fromOff uint64
	toSect, toOff     uint64
}

// AnalyzeSplitPageGroups proves which ARM64_OFF12 references share an ADRP
// inside the given code ranges and evaluates whether each proved group still
// encodes under newBases, an optional map from one-based section index (zero
// for the image header) to a hypothetical new virtual base address. Sections
// absent from newBases keep their current base.
//
// The analysis is purely static: instructions are decoded and read, but they
// are never modified or executed, Export is not called, and the File gains no
// persistent state. Only ARM64_ADRP and ARM64_OFF12 split-info references
// participate; every other kind is left to the existing interfaces.
func (f *File) AnalyzeSplitPageGroups(ranges []SplitPageCodeRange, newBases map[int]uint64) (*SplitPageReport, error) {
	if f.CPU != types.CPUArm64 && f.CPU != types.CPUArm6432 {
		return nil, fmt.Errorf("AnalyzeSplitPageGroups requires an ARM64 image, got CPU %s", f.CPU)
	}
	if err := f.validateSplitPageBases(newBases); err != nil {
		return nil, err
	}

	normalized, err := f.validateSplitPageRanges(ranges)
	if err != nil {
		return nil, err
	}

	meta, err := f.readSplitPageMetaRefs()
	if err != nil {
		return nil, err
	}
	if err := f.validateSplitPageMetaRefs(meta, normalized, newBases); err != nil {
		return nil, err
	}

	nodes, entryByRange, err := f.buildSplitPageGraph(normalized, meta)
	if err != nil {
		return nil, err
	}
	f.analyzeSplitPageFlow(nodes, entryByRange)

	return f.buildSplitPageReport(meta, newBases, nodes)
}

func (f *File) validateSplitPageBases(newBases map[int]uint64) error {
	for sect := range newBases {
		if sect < 0 || sect > len(f.Sections) {
			return fmt.Errorf("new base address references unknown section index %d", sect)
		}
	}
	return nil
}

// validateSplitPageRanges copies and sorts the ranges, proves each interval
// is aligned and fully file-backed, and rejects overlapping intervals.
func (f *File) validateSplitPageRanges(ranges []SplitPageCodeRange) ([]SplitPageCodeRange, error) {
	if len(ranges) == 0 {
		return nil, fmt.Errorf("AnalyzeSplitPageGroups requires at least one code range")
	}
	normalized := append([]SplitPageCodeRange(nil), ranges...)
	slices.SortFunc(normalized, func(a, b SplitPageCodeRange) int {
		if a.SectionIndex != b.SectionIndex {
			return a.SectionIndex - b.SectionIndex
		}
		switch {
		case a.Offset < b.Offset:
			return -1
		case a.Offset > b.Offset:
			return 1
		default:
			return 0
		}
	})
	for i := range normalized {
		r := normalized[i]
		if r.SectionIndex < 1 || r.SectionIndex > len(f.Sections) {
			return nil, fmt.Errorf("code range references invalid section index %d (zero is the image header and cannot hold code)", r.SectionIndex)
		}
		if r.Length == 0 {
			return nil, fmt.Errorf("code range in section %d has zero length", r.SectionIndex)
		}
		if r.Length%4 != 0 {
			return nil, fmt.Errorf("code range section %d offset %#x length %#x is not a multiple of four", r.SectionIndex, r.Offset, r.Length)
		}
		sec := f.Sections[r.SectionIndex-1]
		if sec.Flags.IsZerofill() || sec.Flags.IsGbZerofill() || sec.Flags.IsThreadLocalZerofill() {
			return nil, fmt.Errorf("code range section %d (%s.%s) is zero-fill and has no file backing", r.SectionIndex, sec.Seg, sec.Name)
		}
		end := r.Offset + r.Length
		if end < r.Offset || end > sec.Size {
			return nil, fmt.Errorf("code range section %d [%#x,%#x) exceeds section size %#x", r.SectionIndex, r.Offset, end, sec.Size)
		}
		addr := sec.Addr + r.Offset
		if !f.hasFileData(addr, r.Length) {
			return nil, fmt.Errorf("code range section %d [%#x,%#x) is not fully backed by file data", r.SectionIndex, r.Offset, end)
		}
		if i > 0 {
			prev := normalized[i-1]
			if prev.SectionIndex == r.SectionIndex && r.Offset < prev.Offset+prev.Length {
				return nil, fmt.Errorf("code ranges in section %d overlap: [%#x,%#x) and [%#x,%#x)",
					r.SectionIndex, prev.Offset, prev.Offset+prev.Length, r.Offset, end)
			}
		}
	}
	return normalized, nil
}

// readSplitPageMetaRefs decodes LC_SEGMENT_SPLIT_INFO v2, merges exact
// duplicate references, and rejects conflicting metadata for one source.
func (f *File) readSplitPageMetaRefs() ([]spgMetaRef, error) {
	type metaKey struct {
		kind              types.SplitInfoKind
		fromSect, fromOff uint64
		toSect, toOff     uint64
	}
	type sourceKey struct {
		kind     types.SplitInfoKind
		fromSect uint64
		fromOff  uint64
	}
	seen := make(map[metaKey]struct{})
	targets := make(map[sourceKey]spgMetaRef)
	var refs []spgMetaRef
	var conflict error
	err := f.ForEachV2SplitSegReference(func(fromSectionIndex, fromSectionOffset, toSectionIndex, toSectionOffset uint64, kind types.SplitInfoKind) {
		if conflict != nil {
			return
		}
		if kind != types.DYLD_CACHE_ADJ_V2_ARM64_ADRP && kind != types.DYLD_CACHE_ADJ_V2_ARM64_OFF12 {
			return // other references keep being handled by the existing interfaces
		}
		key := metaKey{kind, fromSectionIndex, fromSectionOffset, toSectionIndex, toSectionOffset}
		if _, ok := seen[key]; ok {
			return // identical reference emitted more than once: merge
		}
		seen[key] = struct{}{}
		src := sourceKey{kind, fromSectionIndex, fromSectionOffset}
		if existing, ok := targets[src]; ok && (existing.toSect != toSectionIndex || existing.toOff != toSectionOffset) {
			conflict = fmt.Errorf("conflicting LC_SEGMENT_SPLIT_INFO metadata for %s source %d:%#x: targets %d:%#x and %d:%#x",
				kind, fromSectionIndex, fromSectionOffset, existing.toSect, existing.toOff, toSectionIndex, toSectionOffset)
			return
		}
		targets[src] = spgMetaRef{kind, fromSectionIndex, fromSectionOffset, toSectionIndex, toSectionOffset}
		refs = append(refs, spgMetaRef{kind, fromSectionIndex, fromSectionOffset, toSectionIndex, toSectionOffset})
	})
	if err != nil {
		return nil, err
	}
	if conflict != nil {
		return nil, conflict
	}
	slices.SortFunc(refs, func(a, b spgMetaRef) int {
		switch {
		case a.fromSect != b.fromSect:
			return int(a.fromSect) - int(b.fromSect)
		case a.fromOff != b.fromOff:
			return int(a.fromOff) - int(b.fromOff)
		case a.toSect != b.toSect:
			return int(a.toSect) - int(b.toSect)
		default:
			return int(a.toOff) - int(b.toOff)
		}
	})
	return refs, nil
}

func (f *File) validateSplitPageMetaRefs(refs []spgMetaRef, ranges []SplitPageCodeRange, newBases map[int]uint64) error {
	covered := make(map[[2]uint64]struct{})
	for _, r := range ranges {
		sect := uint64(r.SectionIndex)
		for off := r.Offset; off < r.Offset+r.Length; off += 4 {
			covered[[2]uint64{sect, off}] = struct{}{}
		}
	}
	for _, ref := range refs {
		if ref.fromSect == 0 {
			return fmt.Errorf("%s reference at section offset %#x cannot use the image header (section 0) as a code source", ref.kind, ref.fromOff)
		}
		if ref.fromSect > uint64(len(f.Sections)) {
			return fmt.Errorf("%s source section index %d is out of range (image has %d sections)", ref.kind, ref.fromSect, len(f.Sections))
		}
		if _, ok := covered[[2]uint64{ref.fromSect, ref.fromOff}]; !ok {
			return fmt.Errorf("%s source %d:%#x is not covered by any analyzed code range", ref.kind, ref.fromSect, ref.fromOff)
		}
		if _, err := f.splitPageOldVM(ref.toSect, ref.toOff); err != nil {
			return fmt.Errorf("%s target %d:%#x is invalid: %w", ref.kind, ref.toSect, ref.toOff, err)
		}
		if _, err := f.splitPageNewVM(ref.toSect, ref.toOff, newBases); err != nil {
			return fmt.Errorf("%s target %d:%#x is invalid: %w", ref.kind, ref.toSect, ref.toOff, err)
		}
	}
	return nil
}

// splitPageOldVM resolves a one-based split-info section index (zero is the
// image header) to its current virtual address.
func (f *File) splitPageOldVM(sect, off uint64) (uint64, error) {
	if sect == 0 {
		seg := f.Segment("__TEXT")
		if seg == nil {
			return 0, fmt.Errorf("image header target %#x cannot be resolved without a __TEXT segment", off)
		}
		if off > seg.Memsz {
			return 0, fmt.Errorf("image header target offset %#x exceeds __TEXT size %#x", off, seg.Memsz)
		}
		return f.GetBaseAddress() + off, nil
	}
	if sect > uint64(len(f.Sections)) {
		return 0, fmt.Errorf("section index %d is out of range (image has %d sections)", sect, len(f.Sections))
	}
	sec := f.Sections[sect-1]
	if off > sec.Size {
		return 0, fmt.Errorf("target offset %#x exceeds section %d declared size %#x", off, sect, sec.Size)
	}
	return sec.Addr + off, nil
}

func (f *File) splitPageNewVM(sect, off uint64, newBases map[int]uint64) (uint64, error) {
	if sect == 0 {
		base := f.GetBaseAddress()
		if newBase, ok := newBases[0]; ok {
			base = newBase
		}
		return base + off, nil
	}
	sec := f.Sections[sect-1]
	base := sec.Addr
	if newBase, ok := newBases[int(sect)]; ok {
		base = newBase
	}
	return base + off, nil
}

// buildSplitPageGraph decodes every range into instruction nodes and adds the
// supported control-flow edges: direct B, B.cond, CBZ/CBNZ branches and
// fall-through edges. Unknown instructions keep their fall-through edge but
// mark it as a barrier so provenance never crosses them.
func (f *File) buildSplitPageGraph(ranges []SplitPageCodeRange, refs []spgMetaRef) (map[[2]uint64]*spgNode, []*spgNode, error) {
	consumers := make(map[[2]uint64]struct{})
	for _, ref := range refs {
		if ref.kind == types.DYLD_CACHE_ADJ_V2_ARM64_OFF12 {
			consumers[[2]uint64{ref.fromSect, ref.fromOff}] = struct{}{}
		}
	}

	nodes := make(map[[2]uint64]*spgNode)
	var entries []*spgNode
	for _, r := range ranges {
		sec := f.Sections[r.SectionIndex-1]
		buf := make([]byte, r.Length)
		if _, err := f.cr.ReadAtAddr(buf, sec.Addr+r.Offset); err != nil {
			return nil, nil, fmt.Errorf("failed to read code range section %d [%#x,%#x): %w", r.SectionIndex, r.Offset, r.Offset+r.Length, err)
		}
		var rangeNodes []*spgNode
		for i := uint64(0); i < r.Length; i += 4 {
			off := r.Offset + i
			node := &spgNode{
				sect:     r.SectionIndex,
				off:      off,
				vm:       sec.Addr + off,
				insn:     binary.LittleEndian.Uint32(buf[i:]),
				consumer: false,
			}
			if _, ok := consumers[[2]uint64{uint64(r.SectionIndex), off}]; ok {
				node.consumer = true
				node.base = (node.insn >> 5) & 0x1f
			}
			nodes[[2]uint64{uint64(r.SectionIndex), off}] = node
			rangeNodes = append(rangeNodes, node)
		}
		entries = append(entries, rangeNodes[0])

		for i, node := range rangeNodes {
			insn := node.insn
			switch {
			case isSPGRet(insn):
				continue // RET terminates a path: no successors
			case isSPGBranch(insn):
				// Unconditional B: only the branch edge exists.
				if target := f.splitPageBranchTarget(nodes, node, insn, 26); target != nil {
					node.edges = append(node.edges, spgEdge{to: target})
				}
			case isSPGBranchCond(insn):
				if target := f.splitPageBranchTarget(nodes, node, insn, 19); target != nil {
					node.edges = append(node.edges, spgEdge{to: target})
				}
				f.addSplitPageFallthrough(node, rangeNodes, i)
			case isSPGCompareBranch(insn):
				if target := f.splitPageBranchTarget(nodes, node, insn, 19); target != nil {
					node.edges = append(node.edges, spgEdge{to: target})
				}
				f.addSplitPageFallthrough(node, rangeNodes, i)
			case isSPGKnown(insn):
				f.addSplitPageFallthrough(node, rangeNodes, i)
			default:
				// Unknown instruction: keep the edge structurally but never
				// propagate provenance across it. An unknown instruction may
				// clobber any register, so the edge carries all-barrier state.
				if i+1 < len(rangeNodes) {
					node.edges = append(node.edges, spgEdge{to: rangeNodes[i+1], barrier: true})
				}
			}
		}
	}
	for _, node := range nodes {
		for _, edge := range node.edges {
			edge.to.in = append(edge.to.in, node)
		}
	}
	return nodes, entries, nil
}

func (f *File) addSplitPageFallthrough(node *spgNode, rangeNodes []*spgNode, i int) {
	if i+1 < len(rangeNodes) {
		node.edges = append(node.edges, spgEdge{to: rangeNodes[i+1]})
	}
}

func (f *File) splitPageBranchTarget(nodes map[[2]uint64]*spgNode, node *spgNode, insn uint32, bits uint) *spgNode {
	var imm uint32
	if bits == 26 {
		imm = insn & 0x03ffffff
	} else {
		imm = (insn >> 5) & 0x7ffff
	}
	delta := int64(signExtendSPG(imm, bits)) * 4
	targetOff, underflow := addSignedOffsetSPG(node.off, delta)
	if underflow {
		return nil
	}
	// A miss (including a branch into another section or outside the code
	// ranges) is an analysis barrier: that path leaves the analyzed region.
	return nodes[[2]uint64{uint64(node.sect), targetOff}]
}

func addSignedOffsetSPG(off uint64, delta int64) (uint64, bool) {
	if delta < 0 {
		back := uint64(-delta)
		if back > off {
			return 0, true
		}
		return off - back, false
	}
	return off + uint64(delta), false
}

func signExtendSPG(value uint32, bits uint) uint32 {
	if value&(1<<(bits-1)) != 0 {
		value |= ^uint32(0) << bits
	}
	return value
}

// analyzeSplitPageFlow runs the register-provenance dataflow to a fixed point
// so back edges from loops converge instead of being guessed at.
func (f *File) analyzeSplitPageFlow(nodes map[[2]uint64]*spgNode, entries []*spgNode) {
	isEntry := make(map[*spgNode]struct{}, len(entries))
	for _, e := range entries {
		isEntry[e] = struct{}{}
	}
	worklist := append([]*spgNode(nil), entries...)
	queued := make(map[*spgNode]struct{})
	for _, e := range entries {
		queued[e] = struct{}{}
	}
	for len(worklist) != 0 {
		node := worklist[0]
		worklist = worklist[1:]
		delete(queued, node)

		// Range entries assume every register holds an unprovable runtime
		// value, not bottom: a real path reaching the entry does carry those
		// values, so an alternative in-range ADRP path merging at a join must
		// produce ambiguity rather than silently win.
		var in [32]spgReg
		if _, ok := isEntry[node]; ok {
			for r := range in {
				in[r] = spgReg{prov: provBarrier}
			}
		}
		for _, pred := range node.in {
			predEdge := pred.edgeTo(node)
			if predEdge != nil && predEdge.barrier {
				// A barrier edge models an unknown instruction that may clobber
				// anything: every register arrives unprovable, so a clean
				// alternative path still merges into ambiguity.
				for r := 0; r < 32; r++ {
					in[r] = mergeSPGReg(in[r], spgReg{prov: provBarrier})
				}
				continue
			}
			for r := 0; r < 32; r++ {
				in[r] = mergeSPGReg(in[r], pred.out[r])
			}
		}

		if node.consumer {
			node.result, node.reason = f.evalSplitPageConsumer(node, in)
		}

		out := f.applySplitPageSemantics(node, in)
		if !splitPageStateEqual(out, node.out) {
			node.out = out
			for _, edge := range node.edges {
				if _, ok := queued[edge.to]; !ok {
					queued[edge.to] = struct{}{}
					worklist = append(worklist, edge.to)
				}
			}
		}
	}
}

func (node *spgNode) edgeTo(target *spgNode) *spgEdge {
	for i := range node.edges {
		if node.edges[i].to == target {
			return &node.edges[i]
		}
	}
	return nil
}

func (f *File) evalSplitPageConsumer(node *spgNode, in [32]spgReg) (spgReg, string) {
	if node.base == 31 {
		return spgReg{prov: provAmbiguous}, fmt.Sprintf("base register x31 (SP/zero register) at %d:%#x cannot carry an ADRP source", node.sect, node.off)
	}
	reg := in[node.base]
	switch reg.prov {
	case provADRP:
		node.adrp = reg.adrp
		return reg, ""
	case provOther:
		return spgReg{prov: provAmbiguous}, fmt.Sprintf("base register x%d at %d:%#x is fed by a non-ADRP value", node.base, node.sect, node.off)
	case provBarrier:
		return spgReg{prov: provBarrier}, fmt.Sprintf("base register x%d at %d:%#x is unprovable because a reachable path crosses an analysis barrier or leaves a code range", node.base, node.sect, node.off)
	case provAmbiguous:
		return spgReg{prov: provAmbiguous}, fmt.Sprintf("base register x%d at %d:%#x merges conflicting sources", node.base, node.sect, node.off)
	default:
		return spgReg{prov: provUnknown}, fmt.Sprintf("base register x%d at %d:%#x has no provable source on any reachable path", node.base, node.sect, node.off)
	}
}

// applySplitPageSemantics models the supported instructions. ADRP establishes
// a page source and MOV propagates its source register; every other write is
// an ordinary write-back that overwrites the destination with a non-ADRP
// value, STR writes no register, and X31 never carries provenance. When the
// instruction is itself an OFF12 consumer, the caller reads the base register
// from the incoming state before this write-back is applied.
func (f *File) applySplitPageSemantics(node *spgNode, in [32]spgReg) [32]spgReg {
	out := in
	insn := node.insn
	switch {
	case isSPGADRP(insn):
		rd := insn & 0x1f
		if rd != 31 {
			out[rd] = spgReg{prov: provADRP, adrp: node}
		}
	case isSPGMov(insn):
		rd, rm := insn&0x1f, (insn>>16)&0x1f
		if rd != 31 {
			if rm == 31 {
				out[rd] = spgReg{prov: provOther} // XZR reads as zero
			} else {
				out[rd] = in[rm]
			}
		}
	case isSPGAddImm(insn):
		// ADD is an ordinary write-back: the computed address is a new value,
		// not the ADRP page itself, so it overwrites provenance.
		if rd := insn & 0x1f; rd != 31 {
			out[rd] = spgReg{prov: provOther}
		}
	case isSPGLoadStore(insn):
		if insn&0x00400000 != 0 { // LDR: the loaded value overwrites Rt
			rt := insn & 0x1f
			if rt != 31 {
				out[rt] = spgReg{prov: provOther}
			}
		}
		// STR writes no register; unsigned-offset loads/stores have no
		// base write-back.
	}
	return out
}

func mergeSPGReg(a, b spgReg) spgReg {
	if a.prov == provUnknown {
		return b
	}
	if b.prov == provUnknown {
		return a
	}
	if a.prov == provAmbiguous || b.prov == provAmbiguous {
		return spgReg{prov: provAmbiguous}
	}
	if a.prov == provBarrier || b.prov == provBarrier {
		if a.prov == b.prov {
			return spgReg{prov: provBarrier} // every path blocked: nothing provable
		}
		// One path crosses a barrier (its value could be anything) while
		// another path proved a source: the ADRP cannot be proved, so this
		// is an ambiguous merge rather than an unknown one.
		return spgReg{prov: provAmbiguous}
	}
	if a.prov == provOther || b.prov == provOther {
		if a.prov == provOther && b.prov == provOther {
			return spgReg{prov: provOther}
		}
		return spgReg{prov: provAmbiguous} // one path is not an ADRP source
	}
	if a.adrp == b.adrp {
		return a
	}
	return spgReg{prov: provAmbiguous} // distinct ADRPs merge
}

func splitPageStateEqual(a, b [32]spgReg) bool {
	for i := range a {
		if a[i].prov != b[i].prov || (a[i].prov == provADRP && a[i].adrp != b[i].adrp) {
			return false
		}
	}
	return true
}

// buildSplitPageReport turns the converged dataflow into the sorted report,
// verifies each ADRP/OFF12 pair against the metadata-declared target, and
// evaluates the groups against the hypothetical section bases.
func (f *File) buildSplitPageReport(meta []spgMetaRef, newBases map[int]uint64, nodes map[[2]uint64]*spgNode) (*SplitPageReport, error) {
	metaTarget := make(map[[2]uint64]spgMetaRef) // source (sect,off) -> OFF12 metadata
	for _, ref := range meta {
		if ref.kind == types.DYLD_CACHE_ADJ_V2_ARM64_OFF12 {
			metaTarget[[2]uint64{ref.fromSect, ref.fromOff}] = ref
		}
	}

	groups := make(map[*spgNode]*SplitPageGroup)
	var reportRefs []*SplitPageReference
	for key, node := range nodes {
		if !node.consumer {
			continue
		}
		ref := metaTarget[key]
		oldSourceVM, err := f.splitPageOldVM(uint64(node.sect), node.off)
		if err != nil {
			return nil, err
		}
		oldTargetVM, err := f.splitPageOldVM(ref.toSect, ref.toOff)
		if err != nil {
			return nil, err
		}
		newSourceVM, err := f.splitPageNewVM(uint64(node.sect), node.off, newBases)
		if err != nil {
			return nil, err
		}
		newTargetVM, err := f.splitPageNewVM(ref.toSect, ref.toOff, newBases)
		if err != nil {
			return nil, err
		}
		rec := &SplitPageReference{
			Kind:                types.DYLD_CACHE_ADJ_V2_ARM64_OFF12,
			SourceSectionIndex:  node.sect,
			SourceSectionOffset: node.off,
			TargetSectionIndex:  int(ref.toSect),
			TargetSectionOffset: ref.toOff,
			SourceVmAddr:        oldSourceVM,
			TargetVmAddr:        oldTargetVM,
			NewSourceVmAddr:     newSourceVM,
			NewTargetVmAddr:     newTargetVM,
		}
		switch node.result.prov {
		case provADRP:
			group, ok := groups[node.adrp]
			if !ok {
				adrpVM, err := f.splitPageOldVM(uint64(node.adrp.sect), node.adrp.off)
				if err != nil {
					return nil, err
				}
				newADRPVM, err := f.splitPageNewVM(uint64(node.adrp.sect), node.adrp.off, newBases)
				if err != nil {
					return nil, err
				}
				group = &SplitPageGroup{
					ADRPSectionIndex:  node.adrp.sect,
					ADRPSectionOffset: node.adrp.off,
					ADRPVmAddr:        adrpVM,
					NewADRPVmAddr:     newADRPVM,
				}
				groups[node.adrp] = group
			}
			rec.Group = group
			group.References = append(group.References, rec)
			if reason, ok := f.checkSplitPageConsistency(node, rec); !ok {
				rec.Status = SplitPageStatusInconsistent
				rec.Reason = reason
			}
		case provAmbiguous:
			rec.Status = SplitPageStatusAmbiguous
			rec.Reason = node.reason
		case provBarrier, provUnknown:
			rec.Status = SplitPageStatusUnknown
			rec.Reason = node.reason
		default:
			rec.Status = SplitPageStatusUnknown
			rec.Reason = fmt.Sprintf("base register x%d at %d:%#x has no provable source on any reachable path", node.base, node.sect, node.off)
		}
		reportRefs = append(reportRefs, rec)
	}

	slices.SortFunc(reportRefs, func(a, b *SplitPageReference) int {
		switch {
		case a.SourceSectionIndex != b.SourceSectionIndex:
			return a.SourceSectionIndex - b.SourceSectionIndex
		case a.SourceSectionOffset != b.SourceSectionOffset:
			return int(a.SourceSectionOffset) - int(b.SourceSectionOffset)
		case a.TargetSectionIndex != b.TargetSectionIndex:
			return a.TargetSectionIndex - b.TargetSectionIndex
		default:
			return int(a.TargetSectionOffset) - int(b.TargetSectionOffset)
		}
	})

	var reportGroups []*SplitPageGroup
	for _, group := range groups {
		consistent := make([]*SplitPageReference, 0, len(group.References))
		hasInconsistent := false
		for _, rec := range group.References {
			if rec.Status == SplitPageStatusInconsistent {
				hasInconsistent = true
				continue
			}
			consistent = append(consistent, rec)
		}
		switch {
		case hasInconsistent:
			// A broken original pair invalidates the group before any
			// hypothetical layout is considered. The consistent members cannot
			// be trusted on their own either, since the shared ADRP no longer
			// reconstructs every declared target.
			for _, rec := range consistent {
				rec.Status = SplitPageStatusInconsistent
				rec.Reason = "group contains an ADRP/OFF12 pair that does not reconstruct its metadata target"
			}
			group.Status = SplitPageStatusInconsistent
		default:
			newADRPPage := group.NewADRPVmAddr &^ uint64(0xfff)
			pages := make(map[uint64]struct{})
			for _, rec := range consistent {
				pages[rec.NewTargetVmAddr&^uint64(0xfff)] = struct{}{}
			}
			split := len(pages) > 1
			anyOverflow := false
			for _, rec := range consistent {
				newTargetPage := rec.NewTargetVmAddr &^ uint64(0xfff)
				pageDistance := int64(newTargetPage>>12) - int64(newADRPPage>>12)
				overflow := pageDistance < minADRPPageCount || pageDistance > maxADRPPageCount
				if overflow {
					anyOverflow = true
				}
				if split || overflow {
					// Every consumer is reported: a split page breaks the
					// shared ADRP for the whole group, not just one member.
					rec.Status = SplitPageStatusConflict
					rec.Reason = f.splitPageConflictReason(rec, newADRPPage, newTargetPage, pageDistance, split, overflow)
				} else {
					rec.Status = SplitPageStatusCompatible
				}
			}
			if split || anyOverflow {
				group.Status = SplitPageStatusConflict
			} else {
				group.Status = SplitPageStatusCompatible
			}
		}
		slices.SortFunc(group.References, func(a, b *SplitPageReference) int {
			if a.SourceSectionIndex != b.SourceSectionIndex {
				return a.SourceSectionIndex - b.SourceSectionIndex
			}
			if a.SourceSectionOffset != b.SourceSectionOffset {
				return int(a.SourceSectionOffset) - int(b.SourceSectionOffset)
			}
			return int(a.TargetSectionOffset) - int(b.TargetSectionOffset)
		})
		reportGroups = append(reportGroups, group)
	}
	slices.SortFunc(reportGroups, func(a, b *SplitPageGroup) int {
		if a.ADRPSectionIndex != b.ADRPSectionIndex {
			return a.ADRPSectionIndex - b.ADRPSectionIndex
		}
		return int(a.ADRPSectionOffset) - int(b.ADRPSectionOffset)
	})

	return &SplitPageReport{References: reportRefs, Groups: reportGroups}, nil
}

func (f *File) splitPageConflictReason(rec *SplitPageReference, newADRPPage, newTargetPage uint64, pageDistance int64, split, overflow bool) string {
	if overflow {
		return fmt.Sprintf("signed ADRP page distance %d from %#x to target %#x exceeds the 21-bit encoding range",
			pageDistance, newADRPPage, newTargetPage)
	}
	return fmt.Sprintf("target section %d moved the target for %d:%#x onto page %#x, split from the ADRP group page %#x",
		rec.TargetSectionIndex, rec.SourceSectionIndex, rec.SourceSectionOffset, newTargetPage, newADRPPage)
}

const (
	minADRPPageCount = -(1 << 20)
	maxADRPPageCount = (1 << 20) - 1
)

// checkSplitPageConsistency confirms the ADRP's signed page displacement plus
// the consumer's encoded immediate and memory access scale reconstruct the
// target declared by LC_SEGMENT_SPLIT_INFO.
func (f *File) checkSplitPageConsistency(node *spgNode, rec *SplitPageReference) (string, bool) {
	adrpNode := node.adrp
	// immhi lives in bits 23:5 (19 bits), immlo in bits 30:29 (2 bits).
	imm := (((adrpNode.insn >> 5) & 0x7ffff) << 2) | ((adrpNode.insn >> 29) & 0x3)
	// Shift the 21-bit value to the top of a 32-bit word so the signed shift
	// performs the sign extension, then shift back into page units.
	pageDelta := int64(int32(imm<<11)) >> 11
	adrpTargetPage := (adrpNode.vm &^ uint64(0xfff)) + uint64(pageDelta)*0x1000

	addend, ok := decodeOff12(node.insn)
	if !ok {
		return fmt.Sprintf("instruction %#08x at %d:%#x is not an encodable OFF12 ADD/LDR/STR", node.insn, node.sect, node.off), false
	}
	if adrpTargetPage+addend != rec.TargetVmAddr {
		return fmt.Sprintf(
			"ADRP %#x page target %#x plus OFF12 addend %#x reconstructs %#x, metadata declares target %#x",
			adrpNode.vm, adrpTargetPage, addend, adrpTargetPage+addend, rec.TargetVmAddr), false
	}
	return "", true
}

func isSPGADRP(insn uint32) bool       { return insn&0x9f000000 == 0x90000000 }
func isSPGRet(insn uint32) bool        { return insn&0xfffffc1f == 0xd65f0000 }
func isSPGBranch(insn uint32) bool     { return insn&0xfc000000 == 0x14000000 } // B only; BL stays an unsupported barrier
func isSPGBranchCond(insn uint32) bool { return insn&0xff000010 == 0x54000000 }
func isSPGCompareBranch(insn uint32) bool {
	return insn&0x7e000000 == 0x34000000 // CBZ/CBNZ, W or X
}
func isSPGMov(insn uint32) bool { return insn&0xffe0ffe0 == 0xaa0003e0 } // MOV Xd, Xm (shift 0)
func isSPGAddImm(insn uint32) bool {
	return insn&0xffc00000 == 0x91000000 // 64-bit ADD immediate, shift 0
}
func isSPGLoadStore(insn uint32) bool {
	_, ok := decodeOff12(insn)
	return ok && insn&0xffc00000 != 0x91000000
}

// isSPGKnown reports whether the instruction is in the supported set. Branches
// are handled by the caller, so this covers the data instructions, NOP and RET.
func isSPGKnown(insn uint32) bool {
	return insn == 0xd503201f || isSPGADRP(insn) || isSPGMov(insn) ||
		isSPGAddImm(insn) || isSPGLoadStore(insn)
}
