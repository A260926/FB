/*Copyright (C) 2024 Mandiant, Inc. All Rights Reserved.*/

package objfile

import (
	"encoding/binary"
	"fmt"

	"github.com/mandiant/GoReSym/debug/gosym"
)

// Runtime pclntab slot numbers, stable for Go 1.18+ _func records.
const (
	pcdataInlTreeIndex uint32 = 2 // PCDATA slot selecting the active inlinedCall record
	funcdataInlTree    uint32 = 3 // FUNCDATA slot holding the function's inlinedCall array
	inlinedCallSize           = 16
)

// inlinedCall mirrors runtime.inlinedCall: 16 bytes, one per inlined call site.
type inlinedCall struct {
	funcID    uint8
	nameOff   int32 // funcnametab offset of the inlined function name
	parentPc  int32 // instruction offset relative to the physical function entry
	startLine int32 // line at which the inlined function was declared
}

// InlineFrames recovers the full inlining chain for an instruction address,
// ordered from the innermost inlined callee out to the physical function.
//
// tab and module must be the final candidates produced by one parse of the
// same File. pc is an actual instruction address; return addresses must be
// normalized by the caller before calling. A function without inline
// information yields a single physical frame. An address outside every known
// function, or any malformed record on the queried chain, returns an error
// rather than a partial chain.
func (e *Entry) InlineFrames(tab *gosym.Table, module *ModuleData, pc uint64) ([]gosym.InlineFrame, error) {
	if tab == nil || tab.Go12line == nil {
		return nil, fmt.Errorf("inline frames: nil pclntab table")
	}
	if module == nil {
		return nil, fmt.Errorf("inline frames: nil moduledata")
	}
	lt := tab.Go12line
	if !lt.SupportsInlineTree() {
		return nil, fmt.Errorf("inline recovery requires a Go 1.18+ 64-bit pclntab")
	}

	fd := lt.FindFuncData(pc)
	if fd == nil {
		return nil, fmt.Errorf("pc %#x is outside any known function", pc)
	}
	entry := fd.EntryPC()

	// Physical frame. Its position comes from the physical function's own
	// pcfile/pcln tables evaluated at the queried instruction. A tail PC past
	// the last pctab step is still inside the functab range; the runtime would
	// report no coordinates for it rather than rejecting the frame, so keep
	// the physical frame with empty/zero coordinates instead of failing.
	physName, ok := lt.FuncNameChecked(fd.NameOff())
	if !ok {
		return nil, fmt.Errorf("illegal function name offset %d for pc %#x", fd.NameOff(), pc)
	}
	physFile, physLine, lineErr := lt.FileLine(fd, pc)
	if lineErr != nil {
		physFile, physLine = "", 0
	}
	physical := gosym.InlineFrame{
		Name:      physName,
		File:      physFile,
		Line:      int(physLine),
		StartLine: int(fd.StartLine()),
		PC:        pc,
		Inlined:   false,
	}

	// No PCDATA table means this instruction cannot name an inlined record.
	pcdataOff, hasPCData := fd.PCDataSlot(pcdataInlTreeIndex)
	if !hasPCData {
		return []gosym.InlineFrame{physical}, nil
	}

	inlIndex, ok := lt.PCValue(pcdataOff, entry, pc)
	if !ok {
		return nil, fmt.Errorf("PCDATA_InlTreeIndex table has no value for pc %#x", pc)
	}
	if inlIndex < 0 {
		// Valid table, but the instruction sits in physical (non-inlined) code.
		return []gosym.InlineFrame{physical}, nil
	}

	// A PCDATA index requires the matching FUNCDATA array. An absent slot
	// (0xffffffff) differs from a present slot at offset zero.
	funcdataOff, hasFuncData := fd.FuncDataSlot(funcdataInlTree)
	if !hasFuncData {
		return nil, fmt.Errorf("PCDATA_InlTreeIndex=%d at pc %#x but FUNCDATA_InlTree is absent", inlIndex, pc)
	}
	if module.GoFunc == 0 {
		return nil, fmt.Errorf("cannot resolve FUNCDATA_InlTree at pc %#x: moduledata.gofunc VA is zero", pc)
	}
	inlTreeVA := module.GoFunc + uint64(funcdataOff)

	// Guard against cycles in malformed parent chains. The chain walks call
	// positions, so a repeated (call PC, record index) pair can never occur
	// in a well-formed tree.
	type chainNode struct {
		pc  uint64
		idx int32
	}
	visited := make(map[chainNode]bool)

	var frames []gosym.InlineFrame
	queryPC := pc
	recordIndex := int32(inlIndex)
	for {
		node := chainNode{queryPC, recordIndex}
		if visited[node] {
			return nil, fmt.Errorf("inlined call tree contains a cycle at record %d (pc %#x)", recordIndex, queryPC)
		}
		visited[node] = true
		if recordIndex < 0 {
			return nil, fmt.Errorf("negative inlinedCall index %d at pc %#x", recordIndex, queryPC)
		}

		raw, err := e.raw.read_memory(inlTreeVA+uint64(recordIndex)*inlinedCallSize, inlinedCallSize)
		if err != nil {
			return nil, fmt.Errorf("short read of inlinedCall record %d at %#x: %w", recordIndex, inlTreeVA+uint64(recordIndex)*inlinedCallSize, err)
		}
		call := parseInlinedCall(raw)

		nameOff := call.nameOff
		if nameOff < 0 {
			return nil, fmt.Errorf("illegal inlinedCall name offset %d in record %d", nameOff, recordIndex)
		}
		name, ok := lt.FuncNameChecked(uint32(nameOff))
		if !ok {
			return nil, fmt.Errorf("illegal inlined function name offset %d in record %d", nameOff, recordIndex)
		}

		// parentPc is an instruction offset inside the physical function, not
		// a parent record number. The frame's file/line are read from the
		// physical function's tables at that call position; the inlined
		// function never gets its own physical _func lookup.
		if call.parentPc < 0 {
			return nil, fmt.Errorf("inlinedCall record %d has negative parentPc %d", recordIndex, call.parentPc)
		}
		callPC := entry + uint64(call.parentPc)
		if fn := tab.PCToFunc(callPC); fn == nil || fn.Entry != entry {
			return nil, fmt.Errorf("inlinedCall record %d parentPc %d escapes its physical function", recordIndex, call.parentPc)
		}
		file, line, err := lt.FileLine(fd, callPC)
		if err != nil {
			return nil, fmt.Errorf("inlined frame %q at pc %#x: %w", name, callPC, err)
		}

		frames = append(frames, gosym.InlineFrame{
			Name:      name,
			File:      file,
			Line:      int(line),
			StartLine: int(call.startLine),
			PC:        callPC,
			Inlined:   true,
		})

		// Re-evaluate the PCDATA selection at the parent call position to find
		// the enclosing record. -1 terminates the chain at the physical frame.
		parentIndex, ok := lt.PCValue(pcdataOff, entry, callPC)
		if !ok {
			return nil, fmt.Errorf("truncated inline chain: no PCDATA value for parent pc %#x", callPC)
		}
		if parentIndex < 0 {
			break
		}
		queryPC = callPC
		recordIndex = parentIndex
	}

	frames = append(frames, physical)
	return frames, nil
}

func parseInlinedCall(raw []byte) inlinedCall {
	// All in-scope images are little-endian 64-bit.
	return inlinedCall{
		funcID:    raw[0],
		nameOff:   int32(binary.LittleEndian.Uint32(raw[4:])),
		parentPc:  int32(binary.LittleEndian.Uint32(raw[8:])),
		startLine: int32(binary.LittleEndian.Uint32(raw[12:])),
	}
}

// InlineFrames delegates to the single entry of an opened object file.
func (f *File) InlineFrames(tab *gosym.Table, module *ModuleData, pc uint64) ([]gosym.InlineFrame, error) {
	return f.entries[0].InlineFrames(tab, module, pc)
}
