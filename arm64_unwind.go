// Copyright 2026 Saferwall. All rights reserved.
// Use of this source code is governed by Apache v2 license
// license that can be found in the LICENSE file.

package pe

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

// ARM64 exception handling (.pdata/.xdata) support.
//
// Reference: "ARM64 Exception Handling", Microsoft Learn:
// https://learn.microsoft.com/en-us/cpp/build/arm64-exception-handling
//
// This file only interprets the static unwind metadata embedded in a fixed
// PE image. It never reads a process stack, never restores register state,
// never executes instructions and never invokes a language handler. A language
// handler is therefore recorded only by its RVA and by the start RVA of its
// unknown-length data blob; the end of that blob is deliberately not guessed.

const (
	// arm64RuntimeFunctionSize is the size in bytes of an ARM64
	// RUNTIME_FUNCTION entry in the exception directory.
	arm64RuntimeFunctionSize = 8

	// ARM64 instructions are always four bytes wide.
	arm64InstrSize = 4
)

// ARM64UnwindPhase identifies where a queried PC lies inside a function.
type ARM64UnwindPhase uint8

const (
	// ARM64PhaseUnknown is the zero value and must never be returned for a
	// successful query.
	ARM64PhaseUnknown ARM64UnwindPhase = 0

	// ARM64PhaseProlog means the PC points at an instruction of the physical
	// prolog that has not executed yet.
	ARM64PhaseProlog ARM64UnwindPhase = 1

	// ARM64PhaseBody means the PC points at an instruction in the function
	// body (including phantom prolog material after an end_c terminator).
	ARM64PhaseBody ARM64UnwindPhase = 2

	// ARM64PhaseEpilog means the PC points at an instruction of a physical
	// epilog that has not executed yet.
	ARM64PhaseEpilog ARM64UnwindPhase = 3
)

// String returns the textual phase name used in forensic reports.
func (p ARM64UnwindPhase) String() string {
	switch p {
	case ARM64PhaseProlog:
		return "prolog"
	case ARM64PhaseBody:
		return "body"
	case ARM64PhaseEpilog:
		return "epilog"
	default:
		return "unknown"
	}
}

// Canonical ARM64 unwind operation names. The names match the opcode names
// used by the ARM64 exception handling documentation.
const (
	arm64OpAllocS      = "alloc_s"
	arm64OpAllocM      = "alloc_m"
	arm64OpAllocL      = "alloc_l"
	arm64OpSaveR19R20X = "save_r19r20_x"
	arm64OpSaveFplr    = "save_fplr"
	arm64OpSaveFplrX   = "save_fplr_x"
	arm64OpSaveReg     = "save_reg"
	arm64OpSaveRegP    = "save_reg_p"
	arm64OpSaveRegX    = "save_reg_x"
	arm64OpSaveRegPX   = "save_reg_p_x"
	arm64OpSaveLRPair  = "save_lrpair"
	arm64OpSaveFreg    = "save_freg"
	arm64OpSaveFregP   = "save_freg_p"
	arm64OpSaveFregX   = "save_freg_x"
	arm64OpSaveFregPX  = "save_freg_p_x"
	arm64OpSaveNext    = "save_next"
	arm64OpSaveNextX   = "save_next_x"
	arm64OpNop         = "nop"
	arm64OpEnd         = "end"
	arm64OpEndC        = "end_c"
	arm64OpSetFp       = "set_fp"
	arm64OpAddFp       = "add_fp"
	arm64OpPacSignLR   = "pac_sign_lr"
)

// ARM64UnwindOp is one canonical unwind operation. Saves are described with
// the register(s), the SP-relative offset and the number of real instructions
// the operation represents, together with the location of the bytes it was
// decoded from. Operations synthesized from a packed .pdata record are marked
// with Synthetic=true.
type ARM64UnwindOp struct {
	// Kind is the canonical opcode name (see the arm64Op* constants).
	Kind string `json:"kind"`

	// Register is the primary register (for example "x19", "d8", "fp", "lr").
	// It is empty for operations that do not name a register (alloc_*, nop,
	// end, end_c, set_fp, add_fp, pac_sign_lr).
	Register string `json:"register,omitempty"`

	// Register2 is the second register of a pair operation (for example
	// "x20" for an x19/x20 pair, "d9" for a d8/d9 pair).
	Register2 string `json:"register2,omitempty"`

	// Offset is the stack offset in bytes relative to the SP value in effect
	// for the save. For writeback (_x) operations it is the slot offset once
	// the frame has been established.
	Offset uint32 `json:"offset,omitempty"`

	// Writeback is the number of bytes the pre-indexed store subtracts from
	// SP. It is zero for non-writeback operations.
	Writeback uint32 `json:"writeback,omitempty"`

	// AllocSize is the stack allocation size in bytes for alloc_* operations.
	AllocSize uint32 `json:"alloc_size,omitempty"`

	// InstrCount is the number of physical ARM64 instructions represented by
	// this operation. Terminator markers (end, end_c) have zero instructions.
	InstrCount uint32 `json:"instr_count"`

	// ByteSpan is the number of source bytes the encoding occupies: code bytes
	// (including operand words) for full xdata, or the 8-byte packed .pdata
	// record for synthesized operations.
	ByteSpan uint32 `json:"byte_span"`

	// SourceRVA is the RVA of the source record: the .pdata entry RVA for
	// packed records, or the RVA inside the .xdata code pool for full xdata.
	SourceRVA uint32 `json:"source_rva"`

	// SourceOffset is the file offset corresponding to SourceRVA.
	SourceOffset uint32 `json:"source_offset"`

	// Synthetic marks operations synthesized from a packed .pdata record.
	// There is no physical unwind code byte stream behind them.
	Synthetic bool `json:"synthetic"`

	// Phantom marks operations found after an end_c terminator. They describe
	// frame setup that still applies to the body but that is not part of the
	// physical prolog of this fragment.
	Phantom bool `json:"phantom,omitempty"`
}

// ARM64UnwindScope is one record of the xdata scope table. Begin and end are
// kept both in the on-disk instruction unit and in bytes.
type ARM64UnwindScope struct {
	// BeginInstruction is the scope start as stored: an instruction index
	// relative to the function start.
	BeginInstruction uint32 `json:"begin_instruction"`

	// EndInstruction is the exclusive scope end in instruction units.
	EndInstruction uint32 `json:"end_instruction"`

	// BeginRVA / EndRVA are the half-open byte range inside the function.
	BeginRVA uint32 `json:"begin_rva"`
	EndRVA   uint32 `json:"end_rva"`

	// HandlerRVA is the language-specific handler RVA (never invoked here).
	HandlerRVA uint32 `json:"handler_rva"`

	// JumpTargetRVA is the jump target as an RVA inside the function.
	JumpTargetRVA uint32 `json:"jump_target_rva"`

	// SourceRVA / SourceOffset locate the scope record in the image.
	SourceRVA    uint32 `json:"source_rva"`
	SourceOffset uint32 `json:"source_offset"`
}

// ARM64UnwindFunction is the decoded view of one function table entry.
type ARM64UnwindFunction struct {
	// BeginRVA / EndRVA delimit the half-open function range [BeginRVA, EndRVA).
	BeginRVA uint32 `json:"begin_rva"`
	EndRVA   uint32 `json:"end_rva"`

	// RecordKind is one of "packed_flag1", "packed_flag2" or "full_xdata".
	RecordKind string `json:"record_kind"`

	// PDatARVA / PDatAOffset locate the 8-byte .pdata entry.
	PDatARVA    uint32 `json:"pdata_rva"`
	PDatAOffset uint32 `json:"pdata_offset"`

	// XDATARVA / XDATAOffset locate the xdata record; zero for packed records.
	XDATARVA    uint32 `json:"xdat_a_rva,omitempty"`
	XDATAOffset uint32 `json:"xdat_a_offset,omitempty"`

	// PrologOps are the canonical prolog operations in execution order.
	// Operations after an end_c terminator carry Phantom=true.
	PrologOps []ARM64UnwindOp `json:"prolog_ops"`

	// PhysicalPrologBytes is the size of the real prolog. Phantom operations
	// after end_c are excluded.
	PhysicalPrologBytes uint32 `json:"physical_prolog_bytes"`

	// EpilogStarts are the function-relative byte start offsets of every
	// epilog. A compressed (E bit) epilog starts right after the prolog.
	EpilogStarts []uint32 `json:"epilog_starts,omitempty"`

	// Epilogs holds the canonical restore operations of every epilog in
	// epilog execution order, aligned with EpilogStarts.
	Epilogs [][]ARM64UnwindOp `json:"epilogs,omitempty"`

	// Scopes is the decoded scope table (may be empty).
	Scopes []ARM64UnwindScope `json:"scopes,omitempty"`

	// HandlerRVA is the language handler RVA when the X bit is set.
	HandlerRVA uint32 `json:"handler_rva,omitempty"`

	// HandlerDataRVA is the start of the opaque, unknown-length handler data.
	// Its end is intentionally not reconstructed.
	HandlerDataRVA uint32 `json:"handler_data_rva,omitempty"`

	// Fragment is true for packed flag=2 records, which describe a fragment
	// without a physical prolog or epilog.
	Fragment bool `json:"fragment,omitempty"`

	// Unsupported is non-empty when the record uses an encoding this reader
	// cannot interpret (flag=3 chains, custom/SVE opcodes, reserved codes).
	// Such a function must never be reported as a complete frame.
	Unsupported string `json:"unsupported,omitempty"`
}

// ARM64UnwindQuery is the result of a single PC lookup.
type ARM64UnwindQuery struct {
	// QueryRVA is the queried PC, pointing at the instruction that has not
	// executed yet.
	QueryRVA uint32 `json:"query_rva"`

	// Function is the function the PC belongs to.
	Function *ARM64UnwindFunction `json:"function"`

	// Phase is prolog, body or epilog.
	Phase ARM64UnwindPhase `json:"phase"`

	// PendingOps are the canonical unwind operations still required from the
	// queried position, in unwind application order (inverse execution order).
	PendingOps []ARM64UnwindOp `json:"pending_ops"`

	// Complete is true only when PendingOps describes the whole frame. A
	// partially executed prolog/epilog is always incomplete.
	Complete bool `json:"complete"`

	// Note carries evidence caveats, for example a partial frame explanation.
	Note string `json:"note,omitempty"`
}

// ARM64UnwindError locates a decoding failure precisely in the image.
type ARM64UnwindError struct {
	// Context names the structure being parsed (for example "pdata[2]").
	Context string

	// RVA / Offset locate the offending bytes when known.
	RVA    uint32
	Offset uint32

	// Reason is the underlying error.
	Reason error
}

func (e *ARM64UnwindError) Error() string {
	if e.RVA != 0 || e.Offset != 0 {
		return fmt.Sprintf("arm64 unwind: %s at rva=%#x offset=%#x: %v",
			e.Context, e.RVA, e.Offset, e.Reason)
	}
	return fmt.Sprintf("arm64 unwind: %s: %v", e.Context, e.Reason)
}

func (e *ARM64UnwindError) Unwrap() error { return e.Reason }

// ARM64UnwindView is a reusable, read-only view over the ARM64 unwind
// metadata of a fixed image. It is safe for repeated Lookup calls; queries
// never mutate decoded state, so operation context (for example save_next
// pairs) cannot be polluted by query order.
type ARM64UnwindView struct {
	pe      *File
	dirRVA  uint32
	dirSize uint32
	funcs   []*ARM64UnwindFunction
}

var (
	errARM64NotARM64          = errors.New("image machine is not IMAGE_FILE_MACHINE_ARM64")
	errARM64BadAlignment      = errors.New("RVA is not 4-byte aligned")
	errARM64NoFunction        = errors.New("RVA is outside every function table entry")
	errARM64BadDirectory      = errors.New("exception directory size is not a multiple of 8 bytes")
	errARM64Unmapped          = errors.New("RVA cannot be mapped through section table")
	errARM64ReservedFrameCode = errors.New("reserved packed frame size code (24-31)")
	errARM64BadPackedFrame    = errors.New("packed frame size is smaller than the saved register slots")
	errARM64BadVersion        = errors.New("xdata version bit is not 0")
	errARM64BadExtendedHeader = errors.New("illegal extended xdata header")
	errARM64Unterminated      = errors.New("unwind code sequence is not terminated by end/end_c")
	errARM64UnknownOpcode     = errors.New("unknown or custom/SVE unwind opcode")
	errARM64SaveNextContext   = errors.New("save_next without a preceding register pair")
	errARM64BadScope          = errors.New("scope table record is invalid")
	errARM64Overlap           = errors.New("function table entries overlap")
)

// ARM64UnwindView builds the independent ARM64 unwind view. It validates every
// .pdata entry and, for full records, every referenced xdata structure. Any
// structural problem (overlapping functions, unterminated sequences, illegal
// extended headers, scopes landing in the middle of an operation, ...) returns
// a location-bearing ARM64UnwindError and no partial view is published.
// Records that use explicitly unsupported encodings (packed flag=3 parent
// links, custom/SVE opcodes) are kept in the view with an Unsupported reason;
// querying them fails loudly instead of producing an empty fake frame.
func (pe *File) ARM64UnwindView() (*ARM64UnwindView, error) {

	if pe.NtHeader.FileHeader.Machine != ImageFileMachineARM64 {
		return nil, &ARM64UnwindError{Context: "coff header", Reason: errARM64NotARM64}
	}

	oh64, ok := pe.NtHeader.OptionalHeader.(ImageOptionalHeader64)
	if !ok {
		return nil, &ARM64UnwindError{Context: "optional header", Reason: errARM64NotARM64}
	}
	dir := oh64.DataDirectory[ImageDirectoryEntryException]
	if dir.VirtualAddress == 0 || dir.Size == 0 {
		return nil, &ARM64UnwindError{Context: "exception directory",
			Reason: errors.New("image has no exception directory")}
	}
	if dir.Size%arm64RuntimeFunctionSize != 0 {
		return nil, &ARM64UnwindError{
			Context: "exception directory", RVA: dir.VirtualAddress,
			Reason: errARM64BadDirectory,
		}
	}

	dirOff, err := pe.mappedOffset(dir.VirtualAddress)
	if err != nil {
		return nil, &ARM64UnwindError{Context: "exception directory",
			RVA: dir.VirtualAddress, Reason: err}
	}

	view := &ARM64UnwindView{pe: pe, dirRVA: dir.VirtualAddress, dirSize: dir.Size}
	entryCount := dir.Size / arm64RuntimeFunctionSize
	for i := uint32(0); i < entryCount; i++ {
		entryRVA := dir.VirtualAddress + i*arm64RuntimeFunctionSize
		entryOff := dirOff + i*arm64RuntimeFunctionSize
		fn, perr := pe.parseARM64RuntimeFunction(i, entryRVA, entryOff)
		if perr != nil {
			return nil, perr
		}
		view.funcs = append(view.funcs, fn)
	}

	// Function ranges must be disjoint; an overlap makes per-PC attribution
	// ambiguous, so reject the whole image.
	for i := 0; i < len(view.funcs); i++ {
		for j := i + 1; j < len(view.funcs); j++ {
			a, b := view.funcs[i], view.funcs[j]
			if a.BeginRVA < b.EndRVA && b.BeginRVA < a.EndRVA {
				return nil, &ARM64UnwindError{
					Context: fmt.Sprintf("pdata[%d]/pdata[%d]", i, j),
					RVA:     b.PDatARVA, Offset: b.PDatAOffset,
					Reason: fmt.Errorf("%w: [%#x,%#x) intersects [%#x,%#x)",
						errARM64Overlap, a.BeginRVA, a.EndRVA, b.BeginRVA, b.EndRVA),
				}
			}
		}
	}

	return view, nil
}

// mappedOffset resolves an RVA to a file offset through the real section
// mapping and rejects RVAs that the section table cannot cover.
func (pe *File) mappedOffset(rva uint32) (uint32, error) {
	off := pe.GetOffsetFromRva(rva)
	if off == ^uint32(0) || off >= pe.size {
		return 0, errARM64Unmapped
	}
	return off, nil
}

// readAtRVA returns n bytes at the given RVA, going through the section
// mapping every time. Bytes are never borrowed from neighbouring handlers or
// other records.
func (pe *File) readAtRVA(rva, n uint32) ([]byte, error) {
	off, err := pe.mappedOffset(rva)
	if err != nil {
		return nil, err
	}
	if n == 0 {
		return nil, errors.New("zero-length read")
	}
	if uint64(off)+uint64(n) > uint64(pe.size) {
		return nil, ErrOutsideBoundary
	}
	return pe.data[off : off+n], nil
}

func (pe *File) readU32AtRVA(rva uint32) (uint32, error) {
	b, err := pe.readAtRVA(rva, 4)
	if err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint32(b), nil
}

// parseARM64RuntimeFunction decodes one 8-byte RUNTIME_FUNCTION entry.
func (pe *File) parseARM64RuntimeFunction(index, entryRVA, entryOff uint32) (
	*ARM64UnwindFunction, error) {

	ctx := fmt.Sprintf("pdata[%d]", index)
	b, err := pe.readAtRVA(entryRVA, arm64RuntimeFunctionSize)
	if err != nil {
		return nil, &ARM64UnwindError{Context: ctx, RVA: entryRVA, Offset: entryOff, Reason: err}
	}
	begin := binary.LittleEndian.Uint32(b[0:4])
	second := binary.LittleEndian.Uint32(b[4:8])
	flag := second & 0x3

	if begin%arm64InstrSize != 0 {
		return nil, &ARM64UnwindError{Context: ctx, RVA: entryRVA, Offset: entryOff,
			Reason: fmt.Errorf("begin address %#x is not 4-byte aligned", begin)}
	}

	fn := &ARM64UnwindFunction{
		BeginRVA:    begin,
		PDatARVA:    entryRVA,
		PDatAOffset: entryOff,
	}

	switch flag {
	case 0:
		fn.RecordKind = "full_xdata"
		xdataRVA := second &^ uint32(3)
		if xdataRVA%4 != 0 {
			return nil, &ARM64UnwindError{Context: ctx, RVA: entryRVA, Offset: entryOff,
				Reason: fmt.Errorf("xdata RVA %#x is not aligned", xdataRVA)}
		}
		fn.XDATARVA = xdataRVA
		fn.XDATAOffset, err = pe.mappedOffset(xdataRVA)
		if err != nil {
			return nil, &ARM64UnwindError{Context: ctx, RVA: entryRVA, Offset: entryOff, Reason: err}
		}
		if err := pe.parseARM64XData(fn); err != nil {
			return nil, err
		}
	case 1, 2:
		if flag == 1 {
			fn.RecordKind = "packed_flag1"
		} else {
			fn.RecordKind = "packed_flag2"
			fn.Fragment = true
		}
		if err := pe.parseARM64Packed(fn, second); err != nil {
			return nil, err
		}
	case 3:
		// Flag=3 is a parent/chained unwind record. It is explicitly
		// unsupported; keep the entry so queries can explain why no evidence
		// is available instead of silently dropping the function.
		fn.RecordKind = "packed_flag3"
		fn.EndRVA = begin // length unknown; mark as empty range
		fn.Unsupported = "packed unwind flag=3 (chained/parent unwind info) is unsupported"
	default:
		panic("unreachable")
	}

	if fn.EndRVA <= fn.BeginRVA && flag != 3 {
		return nil, &ARM64UnwindError{Context: ctx, RVA: entryRVA, Offset: entryOff,
			Reason: fmt.Errorf("function length is not positive: [%#x,%#x)",
				fn.BeginRVA, fn.EndRVA)}
	}
	if flag != 3 && fn.EndRVA%arm64InstrSize != 0 {
		return nil, &ARM64UnwindError{Context: ctx, RVA: entryRVA, Offset: entryOff,
			Reason: fmt.Errorf("end address %#x is not 4-byte aligned", fn.EndRVA)}
	}
	return fn, nil
}

// packed frame size code decoding.
func arm64PackedFrameSize(code uint32) (kind string, size uint32, ok bool) {
	switch {
	case code == 0:
		return "", 0, true
	case code <= 7:
		return arm64OpAllocS, code * 8, true
	case code <= 15:
		return arm64OpAllocM, (code-8)*8 + 512, true
	case code <= 23:
		return arm64OpAllocL, (code-16)*8 + 568, true
	default:
		return "", 0, false
	}
}

// parseARM64Packed expands a packed unwind word into the canonical sequence
// described in the "Packed unwind data" section of the specification. Flag=2
// fragments reuse exactly the same expansion but own no physical prolog or
// epilog.
func (pe *File) parseARM64Packed(fn *ARM64UnwindFunction, word uint32) error {

	functionLengthWords := (word >> 2) & 0x7ff
	regI := (word >> 13) & 0x3
	hcr := (word >> 15) & 0x1 // H for flag=1, CR for flag=2
	regF := (word >> 16) & 0x7
	frameCode := (word >> 19) & 0x1f

	fn.EndRVA = fn.BeginRVA + functionLengthWords*arm64InstrSize

	allocKind, frameSize, ok := arm64PackedFrameSize(frameCode)
	if !ok {
		return &ARM64UnwindError{
			Context: fmt.Sprintf("packed pdata rva=%#x", fn.PDatARVA),
			RVA:     fn.PDatARVA, Offset: fn.PDatAOffset,
			Reason: fmt.Errorf("%w: code=%d", errARM64ReservedFrameCode, frameCode),
		}
	}

	intBytes := uint32(0)
	switch regI {
	case 0:
	case 1:
		intBytes = 8 // x19
	case 2:
		intBytes = 16 // x19-x20
	case 3:
		intBytes = 32 // x19-x22 (the encoding skips a three-register form)
	}
	// Each saved floating-point register occupies an 8-byte slot.
	fpBytes := regF * 8
	pairBytes := intBytes + fpBytes + hcr*16
	if frameSize < pairBytes {
		return &ARM64UnwindError{
			Context: fmt.Sprintf("packed pdata rva=%#x", fn.PDatARVA),
			RVA:     fn.PDatARVA, Offset: fn.PDatAOffset,
			Reason: fmt.Errorf("%w: frame=%d saved_slots=%d",
				errARM64BadPackedFrame, frameSize, pairBytes),
		}
	}

	synth := func(kind, reg, reg2 string, off, writeback, alloc, instrs uint32) ARM64UnwindOp {
		return ARM64UnwindOp{
			Kind: kind, Register: reg, Register2: reg2, Offset: off,
			Writeback: writeback, AllocSize: alloc, InstrCount: instrs,
			ByteSpan: arm64RuntimeFunctionSize, SourceRVA: fn.PDatARVA,
			SourceOffset: fn.PDatAOffset, Synthetic: true,
		}
	}

	var ops []ARM64UnwindOp

	// Canonical ordering and slot layout (all offsets are relative to the
	// final SP). A pre-indexed pair, when present, occupies the lowest slots
	// and performs the SP adjustment itself.
	switch {
	case regI >= 2:
		// save_r19r20_x: stp x19,x20,[sp,#-frameSize]! -> pair lands at SP+0.
		ops = append(ops, synth(arm64OpSaveR19R20X, "x19", "x20",
			0, frameSize, 0, 1))
		if regI == 3 {
			// RegI=3 saves four integer registers: the pre-indexed pair
			// above plus the x21/x22 pair at the next 16-byte slot.
			ops = append(ops, synth(arm64OpSaveRegP, "x21", "x22", 16, 0, 0, 1))
		}
	case regI == 1:
		// No pre-indexed pair: x19 is saved after the frame is allocated.
		ops = append(ops, synth(arm64OpSaveReg, "x19", "", frameSize-8, 0, 0, 1))
	}

	// Floating-point saves grow upward from slot 0; a possible single
	// register occupies the last 8-byte slot.
	fpSlotBase := intBytes
	for i := uint32(0); i < regF/2; i++ {
		ops = append(ops, synth(arm64OpSaveFregP,
			fmt.Sprintf("d%d", 8+2*i), fmt.Sprintf("d%d", 9+2*i),
			fpSlotBase+16*i, 0, 0, 1))
	}
	if regF&1 == 1 {
		ops = append(ops, synth(arm64OpSaveFreg,
			fmt.Sprintf("d%d", 8+regF-1), "",
			fpSlotBase+16*(regF/2), 0, 0, 1))
	}

	// FP/LR sits directly above the other saved registers. It is a
	// pre-indexed save only when no earlier pair performed the writeback.
	if hcr == 1 {
		off := intBytes + fpBytes
		if regI >= 2 {
			ops = append(ops, synth(arm64OpSaveFplr, "fp", "lr", off, 0, 0, 1))
		} else {
			ops = append(ops, synth(arm64OpSaveFplrX, "fp", "lr", 0, 16, 0, 1))
		}
	}

	// The remaining allocation is performed by a single alloc_* operation.
	// A pre-indexed save_r19r20_x already subtracted the whole frame, so no
	// allocation opcode remains in that case. Otherwise a pre-indexed
	// save_fplr_x accounts for 16 of the bytes.
	if frameCode != 0 && regI < 2 {
		remaining := frameSize
		if hcr == 1 {
			remaining -= 16
		}
		instrs := uint32(1)
		if allocKind == arm64OpAllocL {
			instrs = 2
		}
		ops = append(ops, synth(allocKind, "", "", 0, 0, remaining, instrs))
	}

	fn.PrologOps = ops

	// Flag=2 fragments have no physical prolog/epilog; only flag=1 gets a
	// physical range, with the epilog placed at the function end and matching
	// the prolog instruction count.
	if fn.RecordKind == "packed_flag1" {
		fn.PhysicalPrologBytes = arm64OpTotalBytes(ops)
		if fn.PhysicalPrologBytes > fn.EndRVA-fn.BeginRVA {
			return &ARM64UnwindError{Context: ctxName(fn), RVA: fn.PDatARVA,
				Offset: fn.PDatAOffset,
				Reason: errors.New("prolog is longer than the packed function")}
		}
		epilog := arm64ReverseOps(ops)
		fn.EpilogStarts = []uint32{fn.EndRVA - fn.BeginRVA - fn.PhysicalPrologBytes}
		fn.Epilogs = [][]ARM64UnwindOp{epilog}
	}
	return nil
}

func ctxName(fn *ARM64UnwindFunction) string {
	return fmt.Sprintf("pdata rva=%#x", fn.PDatARVA)
}

func arm64OpTotalBytes(ops []ARM64UnwindOp) uint32 {
	var n uint32
	for _, op := range ops {
		n += op.InstrCount * arm64InstrSize
	}
	return n
}

func arm64ReverseOps(in []ARM64UnwindOp) []ARM64UnwindOp {
	out := make([]ARM64UnwindOp, 0, len(in))
	for i := len(in) - 1; i >= 0; i-- {
		out = append(out, in[i])
	}
	return out
}

// xdata header layout constants.
const (
	arm64XDataVersBit    = uint32(1) << 31
	arm64XDataNormalMask = uint32(0x3ffff) // 18-bit function length
	arm64XDataEBit       = uint32(1) << 18
	arm64XDataXBit       = uint32(1) << 19
	arm64XDataEpilogMask = uint32(0xf)
	arm64XDataCodeShift  = 24
)

// parseARM64XData decodes a version-0 full xdata record.
func (pe *File) parseARM64XData(fn *ARM64UnwindFunction) error {

	baseRVA := fn.XDATARVA
	hdr, err := pe.readU32AtRVA(baseRVA)
	if err != nil {
		return &ARM64UnwindError{Context: "xdata header", RVA: baseRVA, Reason: err}
	}
	if hdr&arm64XDataVersBit != 0 {
		return &ARM64UnwindError{Context: "xdata header", RVA: baseRVA,
			Reason: fmt.Errorf("%w: header=%#08x", errARM64BadVersion, hdr)}
	}

	var (
		funcWords   uint32
		eBit, xBit  bool
		epilogCount uint32
		codeWords   uint32
		headerSize  uint32 = 4
	)
	normalEpilogCount := (hdr >> 20) & arm64XDataEpilogMask
	if normalEpilogCount == 0 {
		// Extended header: one more 32-bit word.
		ext, err := pe.readU32AtRVA(baseRVA + 4)
		if err != nil {
			return &ARM64UnwindError{Context: "xdata extended header",
				RVA: baseRVA + 4, Reason: err}
		}
		if ext&arm64XDataVersBit != 0 {
			return &ARM64UnwindError{Context: "xdata extended header",
				RVA: baseRVA + 4,
				Reason: fmt.Errorf("%w: version bit set in %#08x",
					errARM64BadExtendedHeader, ext)}
		}
		funcWords = ext & 0x7fffff // 23 bits
		eBit = ext&(1<<23) != 0
		xBit = ext&(1<<24) != 0
		epilogCount = (ext >> 25) & 0x3f
		if epilogCount == 0 {
			return &ARM64UnwindError{Context: "xdata extended header",
				RVA: baseRVA + 4,
				Reason: fmt.Errorf("%w: epilog count is zero in %#08x",
					errARM64BadExtendedHeader, ext)}
		}
		headerSize = 8
	} else {
		funcWords = hdr & arm64XDataNormalMask
		eBit = hdr&arm64XDataEBit != 0
		xBit = hdr&arm64XDataXBit != 0
		epilogCount = normalEpilogCount
	}
	codeWords = hdr >> arm64XDataCodeShift
	fn.EndRVA = fn.BeginRVA + funcWords*arm64InstrSize

	if eBit && epilogCount != 1 {
		return &ARM64UnwindError{Context: "xdata header", RVA: baseRVA,
			Reason: fmt.Errorf("%w: E bit with epilog count %d",
				errARM64BadExtendedHeader, epilogCount)}
	}

	poolRVA := baseRVA + headerSize
	poolEndRVA := poolRVA + codeWords*4

	// Decode the prolog sequence (bounded strictly by Code Words).
	prolog, prologEnd, physicalProlog, perr := pe.decodeARM64CodeSequence(
		fn, poolRVA, poolEndRVA, true)
	if perr != nil {
		return perr
	}
	fn.PrologOps = prolog
	fn.PhysicalPrologBytes = physicalProlog

	// Phantom operations after end_c still describe the frame for the body.
	for i := range fn.PrologOps {
		if fn.PrologOps[i].Kind == arm64OpEndC {
			for j := i + 1; j < len(fn.PrologOps); j++ {
				fn.PrologOps[j].Phantom = true
			}
			break
		}
	}

	cursor := prologEnd

	if eBit {
		// The single compressed epilog starts with the next code byte.
		epilog, epEnd, _, perr := pe.decodeARM64CodeSequence(fn, cursor, poolEndRVA, false)
		if perr != nil {
			return perr
		}
		fn.EpilogStarts = []uint32{fn.PhysicalPrologBytes}
		fn.Epilogs = [][]ARM64UnwindOp{epilog}
		cursor = epEnd
	} else {
		// Epilog start offsets come first: function-relative byte indices
		// (not instruction indices like scope boundaries), one word per epilog.
		var starts []uint32
		for i := uint32(0); i < epilogCount; i++ {
			if cursor+4 > poolEndRVA {
				return &ARM64UnwindError{
					Context: fmt.Sprintf("epilog start[%d]", i),
					RVA:     cursor, Reason: errARM64Unterminated}
			}
			w, err := pe.readU32AtRVA(cursor)
			if err != nil {
				return &ARM64UnwindError{
					Context: fmt.Sprintf("epilog start[%d]", i),
					RVA:     cursor, Reason: err}
			}
			if w%arm64InstrSize != 0 || w >= funcWords*arm64InstrSize {
				return &ARM64UnwindError{
					Context: fmt.Sprintf("epilog start[%d]", i),
					RVA:     cursor,
					Reason:  fmt.Errorf("epilog start %#x is unaligned or outside the function", w)}
			}
			starts = append(starts, w)
			cursor += 4
		}
		fn.EpilogStarts = starts

		// Each epilog owns an end-terminated byte stream in the shared pool.
		// The pool boundary is enforced, so a stream cannot borrow bytes from
		// a handler or another record.
		for i := uint32(0); i < epilogCount; i++ {
			epilog, epEnd, _, perr := pe.decodeARM64CodeSequence(fn, cursor, poolEndRVA, false)
			if perr != nil {
				return perr
			}
			fn.Epilogs = append(fn.Epilogs, epilog)
			cursor = epEnd
		}
	}

	if cursor > poolEndRVA {
		return &ARM64UnwindError{Context: "xdata code pool", RVA: poolEndRVA,
			Reason: fmt.Errorf("code streams consume %#x bytes but Code Words is %d",
				cursor-poolRVA, codeWords)}
	}

	if xBit {
		if cursor+4 > poolEndRVA {
			return &ARM64UnwindError{Context: "exception handler RVA",
				RVA: cursor, Reason: errARM64Unterminated}
		}
		handler, err := pe.readU32AtRVA(cursor)
		if err != nil {
			return &ARM64UnwindError{Context: "exception handler RVA",
				RVA: cursor, Reason: err}
		}
		fn.HandlerRVA = handler
		cursor += 4

		// The scope table only exists for records with an exception handler
		// (X bit): a count word followed by Count 16-byte records.
		scopeCount, err := pe.readU32AtRVA(cursor)
		if err != nil {
			return &ARM64UnwindError{Context: "scope table count", RVA: cursor, Reason: err}
		}
		cursor += 4
		if scopeCount > 0 {
			if uint64(scopeCount)*16 > uint64(pe.size) {
				return &ARM64UnwindError{Context: "scope table count", RVA: cursor - 4,
					Reason: fmt.Errorf("implausible scope count %d", scopeCount)}
			}
			for i := uint32(0); i < scopeCount; i++ {
				recRVA := cursor + i*16
				rec, err := pe.readAtRVA(recRVA, 16)
				if err != nil {
					return &ARM64UnwindError{Context: fmt.Sprintf("scope[%d]", i),
						RVA: recRVA, Reason: err}
				}
				beginInstr := binary.LittleEndian.Uint32(rec[0:4])
				endInstr := binary.LittleEndian.Uint32(rec[4:8])
				handler := binary.LittleEndian.Uint32(rec[8:12])
				jumpTarget := binary.LittleEndian.Uint32(rec[12:16])

				scope := ARM64UnwindScope{
					BeginInstruction: beginInstr,
					EndInstruction:   endInstr,
					BeginRVA:         fn.BeginRVA + beginInstr*arm64InstrSize,
					EndRVA:           fn.BeginRVA + endInstr*arm64InstrSize,
					HandlerRVA:       handler,
					JumpTargetRVA:    fn.BeginRVA + jumpTarget*arm64InstrSize,
					SourceRVA:        recRVA,
				}
				scope.SourceOffset, _ = pe.mappedOffset(recRVA)

				if beginInstr > endInstr || endInstr > funcWords {
					return &ARM64UnwindError{Context: fmt.Sprintf("scope[%d]", i),
						RVA: recRVA, Offset: scope.SourceOffset,
						Reason: fmt.Errorf("%w: instruction range [%d,%d) outside function of %d words",
							errARM64BadScope, beginInstr, endInstr, funcWords)}
				}
				if jumpTarget >= funcWords {
					return &ARM64UnwindError{Context: fmt.Sprintf("scope[%d]", i),
						RVA: recRVA, Offset: scope.SourceOffset,
						Reason: fmt.Errorf("%w: jump target instruction %d outside function",
							errARM64BadScope, jumpTarget)}
				}
				// A scope boundary must not land in the middle of a physical
				// prolog or epilog operation span.
				if fn.PhysicalPrologBytes > 0 {
					proEndRVA := fn.BeginRVA + fn.PhysicalPrologBytes
					if (scope.BeginRVA > fn.BeginRVA && scope.BeginRVA < proEndRVA) ||
						(scope.EndRVA > fn.BeginRVA && scope.EndRVA < proEndRVA) {
						return &ARM64UnwindError{
							Context: fmt.Sprintf("scope[%d]", i),
							RVA:     recRVA, Offset: scope.SourceOffset,
							Reason: fmt.Errorf("%w: boundary [%#x,%#x) splits prolog [%#x,%#x)",
								errARM64BadScope, scope.BeginRVA, scope.EndRVA,
								fn.BeginRVA, proEndRVA)}
					}
				}
				for epIdx, epStart := range fn.EpilogStarts {
					epBytes := arm64OpTotalBytes(fn.Epilogs[epIdx])
					epStartRVA := fn.BeginRVA + epStart
					epEndRVA := epStartRVA + epBytes
					if (scope.BeginRVA > epStartRVA && scope.BeginRVA < epEndRVA) ||
						(scope.EndRVA > epStartRVA && scope.EndRVA < epEndRVA) {
						return &ARM64UnwindError{
							Context: fmt.Sprintf("scope[%d]", i),
							RVA:     recRVA, Offset: scope.SourceOffset,
							Reason: fmt.Errorf("%w: boundary [%#x,%#x) splits epilog [%#x,%#x)",
								errARM64BadScope, scope.BeginRVA, scope.EndRVA,
								epStartRVA, epEndRVA)}
					}
				}
				fn.Scopes = append(fn.Scopes, scope)
			}
			cursor += scopeCount * 16
		}

		// Everything after the scope table is opaque, unknown-length handler
		// data: record its start only, never speculate about the end.
		fn.HandlerDataRVA = cursor
	}
	return nil
}

// decodeARM64CodeSequence decodes one end/end_c-terminated unwind code stream.
// If hardLimit is non-zero the stream must terminate before it; otherwise the
// section mapping supplies the only bound and a missing terminator is an error.
// It returns the operations, the RVA just past the terminator (rounded up to
// the next 32-bit word) and, for prologs, the physical instruction byte count
// before an end_c/end boundary.
func (pe *File) decodeARM64CodeSequence(fn *ARM64UnwindFunction,
	startRVA, hardLimit uint32, isProlog bool) (
	[]ARM64UnwindOp, uint32, uint32, *ARM64UnwindError) {

	var ops []ARM64UnwindOp
	var physicalBytes uint32
	var afterEndC bool
	rva := startRVA

	// Register-pair context for save_next / save_next_x.
	var (
		havePair      bool
		pairReg       int
		pairOffset    uint32
		pairWriteback bool
	)

	mkOp := func(kind, reg, reg2 string, off, writeback, alloc, instrs, span uint32,
		opRVA uint32) ARM64UnwindOp {
		fileOff, _ := pe.mappedOffset(opRVA)
		return ARM64UnwindOp{
			Kind: kind, Register: reg, Register2: reg2, Offset: off,
			Writeback: writeback, AllocSize: alloc, InstrCount: instrs,
			ByteSpan: span, SourceRVA: opRVA, SourceOffset: fileOff,
		}
	}

	readCodeByte := func(where uint32) (uint8, *ARM64UnwindError) {
		b, err := pe.readAtRVA(where, 1)
		if err != nil {
			return 0, &ARM64UnwindError{
				Context: "xdata code byte", RVA: where, Reason: err}
		}
		return b[0], nil
	}
	readOperand := func(where uint32) (uint32, uint32, *ARM64UnwindError) {
		// Operands always occupy the next whole 32-bit word.
		wordRVA := (where + 4) &^ uint32(3)
		if hardLimit != 0 && wordRVA+4 > hardLimit {
			return 0, 0, &ARM64UnwindError{
				Context: "xdata operand word", RVA: wordRVA,
				Reason: errARM64Unterminated}
		}
		v, err := pe.readU32AtRVA(wordRVA)
		if err != nil {
			return 0, 0, &ARM64UnwindError{
				Context: "xdata operand word", RVA: wordRVA, Reason: err}
		}
		return v, wordRVA + 4, nil
	}

	for {
		if hardLimit != 0 && rva >= hardLimit {
			return nil, 0, 0, &ARM64UnwindError{
				Context: "xdata code pool", RVA: rva, Reason: errARM64Unterminated}
		}
		code, e := readCodeByte(rva)
		if e != nil {
			return nil, 0, 0, e
		}
		opcode := code >> 4
		info := code & 0xf
		opRVA := rva
		rva++

		switch opcode {
		case 0: // alloc_s: size = info * 16
			ops = append(ops, mkOp(arm64OpAllocS, "", "", 0, 0,
				uint32(info)*16, 1, 1, opRVA))
		case 1: // save_r19r20_x: pre-indexed pair, scaled immediate in a word.
			operand, next, e2 := readOperand(rva)
			if e2 != nil {
				return nil, 0, 0, e2
			}
			// stp x19,x20,[sp,#-N]!: after writeback the pair lives at the
			// new SP (slot 0); N is the SP delta.
			ops = append(ops, mkOp(arm64OpSaveR19R20X, "x19", "x20",
				0, operand*8, 0, 1, next-opRVA, opRVA))
			havePair, pairReg, pairOffset, pairWriteback = true, 20, 0, true
			rva = next
		case 2: // save_fplr_x: pre-indexed fp/lr pair, scaled immediate in a word.
			operand, next, e2 := readOperand(rva)
			if e2 != nil {
				return nil, 0, 0, e2
			}
			ops = append(ops, mkOp(arm64OpSaveFplrX, "fp", "lr",
				0, operand*8, 0, 1, next-opRVA, opRVA))
			rva = next
		case 3: // alloc_m: size = (info + 1) * 8
			ops = append(ops, mkOp(arm64OpAllocM, "", "", 0, 0,
				uint32(info+1)*8, 1, 1, opRVA))
		case 4: // save_fplr: offset = info * 8
			ops = append(ops, mkOp(arm64OpSaveFplr, "fp", "lr",
				uint32(info)*8, 0, 0, 1, 1, opRVA))
		case 5, 6, 7, 8: // save_reg / save_reg_p / save_reg_x / save_reg_p_x
			operand, next, e2 := readOperand(rva)
			if e2 != nil {
				return nil, 0, 0, e2
			}
			baseReg := 19 + int(info)
			scaled := operand * 8
			var kind string
			var slotOff, writeback uint32
			switch opcode {
			case 5:
				kind = arm64OpSaveReg
				slotOff = scaled
			case 6:
				kind = arm64OpSaveRegP
				slotOff = scaled
			case 7:
				// Pre-indexed: after the writeback the register sits at SP.
				kind, slotOff, writeback = arm64OpSaveRegX, 0, scaled
			case 8:
				kind, slotOff, writeback = arm64OpSaveRegPX, 0, scaled
			}
			if opcode == 6 || opcode == 8 {
				ops = append(ops, mkOp(kind,
					fmt.Sprintf("x%d", baseReg), fmt.Sprintf("x%d", baseReg+1),
					slotOff, writeback, 0, 1, next-opRVA, opRVA))
				havePair, pairReg, pairOffset, pairWriteback =
					true, baseReg+1, slotOff, opcode == 8
			} else {
				ops = append(ops, mkOp(kind,
					fmt.Sprintf("x%d", baseReg), "", slotOff, writeback, 0, 1,
					next-opRVA, opRVA))
				havePair = false
			}
			rva = next
		case 9: // save_lrpair: pair (lr, x(19+info))
			operand, next, e2 := readOperand(rva)
			if e2 != nil {
				return nil, 0, 0, e2
			}
			off := operand * 8
			ops = append(ops, mkOp(arm64OpSaveLRPair, "lr",
				fmt.Sprintf("x%d", 19+int(info)), off, 0, 0, 1, next-opRVA, opRVA))
			havePair, pairReg, pairOffset, pairWriteback =
				true, 19+int(info), off, false
			rva = next
		case 10: // alloc_l: size = operand * 8, two instructions
			operand, next, e2 := readOperand(rva)
			if e2 != nil {
				return nil, 0, 0, e2
			}
			ops = append(ops, mkOp(arm64OpAllocL, "", "", 0, 0,
				operand*8, 2, next-opRVA, opRVA))
			rva = next
		case 11, 12, 13, 14: // save_freg / save_freg_p / save_freg_x / save_freg_p_x
			operand, next, e2 := readOperand(rva)
			if e2 != nil {
				return nil, 0, 0, e2
			}
			baseReg := 8 + int(info)
			scaled := operand * 8
			var kind string
			var slotOff, writeback uint32
			switch opcode {
			case 11:
				kind = arm64OpSaveFreg
				slotOff = scaled
			case 12:
				kind = arm64OpSaveFregP
				slotOff = scaled
			case 13:
				kind, slotOff, writeback = arm64OpSaveFregX, 0, scaled
			case 14:
				kind, slotOff, writeback = arm64OpSaveFregPX, 0, scaled
			}
			if opcode == 12 || opcode == 14 {
				ops = append(ops, mkOp(kind,
					fmt.Sprintf("d%d", baseReg), fmt.Sprintf("d%d", baseReg+1),
					slotOff, writeback, 0, 1, next-opRVA, opRVA))
			} else {
				ops = append(ops, mkOp(kind,
					fmt.Sprintf("d%d", baseReg), "", slotOff, writeback, 0, 1,
					next-opRVA, opRVA))
			}
			havePair = false
			rva = next
		case 15:
			switch info {
			case 0: // end
				ops = append(ops, mkOp(arm64OpEnd, "", "", 0, 0, 0, 0, 1, opRVA))
				return ops, (rva + 3) &^ uint32(3), physicalBytes, nil
			case 1: // end_c (prolog only)
				if !isProlog {
					return nil, 0, 0, &ARM64UnwindError{
						Context: "xdata epilog", RVA: opRVA,
						Reason: errors.New("end_c is not legal inside an epilog")}
				}
				ops = append(ops, mkOp(arm64OpEndC, "", "", 0, 0, 0, 0, 1, opRVA))
				physicalBytes = arm64OpTotalBytes(ops)
				afterEndC = true
				// Continue: phantom prolog operations follow until end.
			case 2, 3: // save_next / save_next_x
				if !havePair {
					return nil, 0, 0, &ARM64UnwindError{
						Context: "xdata code pool", RVA: opRVA,
						Reason: errARM64SaveNextContext}
				}
				nextReg1 := pairReg + 1
				nextReg2 := pairReg + 2
				nextOff := pairOffset + 16
				kind := arm64OpSaveNext
				var writeback uint32
				if info == 3 {
					kind = arm64OpSaveNextX
					if pairWriteback {
						writeback = 16
					}
				}
				ops = append(ops, mkOp(kind,
					fmt.Sprintf("x%d", nextReg1), fmt.Sprintf("x%d", nextReg2),
					nextOff, writeback, 0, 1, 1, opRVA))
				pairReg, pairOffset = nextReg2, nextOff
			case 4: // nop
				ops = append(ops, mkOp(arm64OpNop, "", "", 0, 0, 0, 1, 1, opRVA))
			case 5: // set_fp
				ops = append(ops, mkOp(arm64OpSetFp, "", "", 0, 0, 0, 1, 1, opRVA))
			case 6: // add_fp
				ops = append(ops, mkOp(arm64OpAddFp, "", "", 0, 0, 0, 1, 1, opRVA))
			case 7: // pac_sign_lr
				ops = append(ops, mkOp(arm64OpPacSignLR, "", "", 0, 0, 0, 1, 1, opRVA))
			default:
				// Custom / SVE encodings (0xF8..0xFF) and any other unknown
				// code: report unsupported, never masquerade as nop.
				reason := fmt.Errorf("%w: opcode byte %#02x",
					errARM64UnknownOpcode, code)
				fn.Unsupported = reason.Error()
				ops = append(ops, mkOp(fmt.Sprintf("unsupported_%#02x", code),
					"", "", 0, 0, 0, 0, 1, opRVA))
				return ops, (rva + 3) &^ uint32(3), physicalBytes,
					&ARM64UnwindError{Context: "xdata code pool",
						RVA: opRVA, Reason: reason}
			}
		default:
			reason := fmt.Errorf("%w: opcode %d", errARM64UnknownOpcode, opcode)
			return nil, 0, 0, &ARM64UnwindError{
				Context: "xdata code pool", RVA: opRVA, Reason: reason}
		}

		// Physical prolog size excludes phantom operations after end_c; it is
		// frozen when end_c was seen and otherwise tracks the decoded stream.
		if isProlog && !afterEndC {
			physicalBytes = arm64OpTotalBytes(ops)
		}
	}
}

// Lookup answers a repeated offline query: given a PC RVA pointing at the
// next instruction to execute, it classifies the position and returns the
// canonical unwind operations still required from there.
func (v *ARM64UnwindView) Lookup(pcRVA uint32) (*ARM64UnwindQuery, error) {

	if pcRVA%arm64InstrSize != 0 {
		return nil, &ARM64UnwindError{Context: "lookup", RVA: pcRVA,
			Reason: errARM64BadAlignment}
	}

	var fn *ARM64UnwindFunction
	for _, candidate := range v.funcs {
		if pcRVA >= candidate.BeginRVA && pcRVA < candidate.EndRVA {
			fn = candidate
			break
		}
	}
	if fn == nil {
		return nil, &ARM64UnwindError{Context: "lookup", RVA: pcRVA,
			Reason: errARM64NoFunction}
	}
	if fn.Unsupported != "" {
		return nil, &ARM64UnwindError{
			Context: fmt.Sprintf("function [%#x,%#x)", fn.BeginRVA, fn.EndRVA),
			RVA:     fn.PDatARVA, Offset: fn.PDatAOffset,
			Reason: errors.New(fn.Unsupported)}
	}

	q := &ARM64UnwindQuery{QueryRVA: pcRVA, Function: fn}
	fullSet := arm64FrameOps(fn.PrologOps)

	switch {
	case fn.Fragment:
		// Flag=2 fragments have no physical prolog/epilog: never trim the
		// canonical operations at the function edges.
		q.Phase = ARM64PhaseBody
		q.PendingOps = arm64ReverseOps(fullSet)
		q.Complete = true
		q.Note = "packed flag=2 fragment; canonical frame operations apply to the whole fragment"

	case pcRVA < fn.BeginRVA+fn.PhysicalPrologBytes:
		q.Phase = ARM64PhaseProlog
		completed := pcRVA - fn.BeginRVA
		executed := arm64ExecutedPrologOps(fn.PrologOps, completed)
		q.PendingOps = arm64ReverseOps(executed)
		q.Complete = len(q.PendingOps) == len(fullSet)
		if !q.Complete {
			q.Note = fmt.Sprintf(
				"partial prolog: %d of %d instruction bytes executed; only completed setup is reversible",
				completed, fn.PhysicalPrologBytes)
		}

	default:
		epIdx := -1
		var epOffset uint32
		for i, start := range fn.EpilogStarts {
			epStartRVA := fn.BeginRVA + start
			epBytes := arm64OpTotalBytes(fn.Epilogs[i])
			if pcRVA >= epStartRVA && pcRVA < epStartRVA+epBytes {
				epIdx = i
				epOffset = start
				break
			}
		}
		if epIdx >= 0 {
			q.Phase = ARM64PhaseEpilog
			completed := pcRVA - (fn.BeginRVA + epOffset)
			epilogFrame := arm64FrameOps(fn.Epilogs[epIdx])
			q.PendingOps = arm64RemainingEpilogOps(fn.Epilogs[epIdx], completed)
			// Completeness is relative to this epilog's own restore sequence:
			// at its first instruction every restore is still required.
			q.Complete = len(q.PendingOps) == len(epilogFrame)
			if !q.Complete {
				q.Note = fmt.Sprintf(
					"partial epilog: %d instruction bytes already restored; %d of %d operations remain",
					completed, len(q.PendingOps), len(epilogFrame))
			}
		} else {
			q.Phase = ARM64PhaseBody
			// Phantom operations after end_c remain required for body unwind.
			q.PendingOps = arm64ReverseOps(fullSet)
			q.Complete = true
		}
	}

	return q, nil
}

// arm64FrameOps returns the frame operations excluding terminator markers.
func arm64FrameOps(prolog []ARM64UnwindOp) []ARM64UnwindOp {
	out := make([]ARM64UnwindOp, 0, len(prolog))
	for _, op := range prolog {
		if op.Kind == arm64OpEnd || op.Kind == arm64OpEndC {
			continue
		}
		out = append(out, op)
	}
	return out
}

// arm64ExecutedPrologOps returns the prolog operations whose instructions have
// fully executed given completedBytes of the physical prolog. Trimming is done
// by represented instruction count, never by source byte span.
func arm64ExecutedPrologOps(prolog []ARM64UnwindOp,
	completedBytes uint32) []ARM64UnwindOp {

	var out []ARM64UnwindOp
	var consumed uint32
	for _, op := range prolog {
		if op.Kind == arm64OpEnd || op.Kind == arm64OpEndC {
			continue
		}
		if op.Phantom {
			// Phantom material is not part of the physical prolog.
			continue
		}
		need := op.InstrCount * arm64InstrSize
		if consumed+need <= completedBytes {
			out = append(out, op)
			consumed += need
		} else {
			break
		}
	}
	return out
}

// arm64RemainingEpilogOps drops restore operations whose instructions have
// already executed, again counting represented instructions, not code-pool
// bytes.
func arm64RemainingEpilogOps(epilog []ARM64UnwindOp, completedBytes uint32) []ARM64UnwindOp {
	var consumed uint32
	var out []ARM64UnwindOp
	for _, op := range epilog {
		if op.Kind == arm64OpEnd || op.Kind == arm64OpEndC {
			continue
		}
		need := op.InstrCount * arm64InstrSize
		if consumed+need <= completedBytes {
			consumed += need
			continue
		}
		out = append(out, op)
		consumed += need
	}
	return out
}

// Functions returns the decoded function records. The returned slice is
// read-only metadata from the fixed image.
func (v *ARM64UnwindView) Functions() []*ARM64UnwindFunction {
	return v.funcs
}

// WriteReport emits a static-evidence report for a batch of offline PC RVAs.
// It never claims a reconstructed call stack: every line is grounded in a
// .pdata/.xdata source location, and partial or unsupported evidence is
// explicitly marked untrusted instead of printing an empty complete frame.
func (v *ARM64UnwindView) WriteReport(w io.Writer, pcRVAs []uint32) error {

	for _, pc := range pcRVAs {
		fmt.Fprintf(w, "PC rva=%#06x\n", pc)
		q, err := v.Lookup(pc)
		if err != nil {
			fmt.Fprintf(w, "  EVIDENCE UNTRUSTED: %v\n", err)
			continue
		}
		fn := q.Function
		fmt.Fprintf(w, "  function [%#06x,%#06x) kind=%s pdata=rva %#x off %#x",
			fn.BeginRVA, fn.EndRVA, fn.RecordKind, fn.PDatARVA, fn.PDatAOffset)
		if fn.XDATARVA != 0 {
			fmt.Fprintf(w, " xdata=rva %#x off %#x", fn.XDATARVA, fn.XDATAOffset)
		}
		fmt.Fprintln(w)
		fmt.Fprintf(w, "  phase=%s complete_frame=%v", q.Phase, q.Complete)
		if q.Note != "" {
			fmt.Fprintf(w, " note=%q", q.Note)
		}
		fmt.Fprintln(w)
		if !q.Complete {
			fmt.Fprintln(w, "  EVIDENCE PARTIAL: not a complete frame; recovery operations below are a subset")
		}
		for _, op := range q.PendingOps {
			origin := "xdata"
			if op.Synthetic {
				origin = "packed-pdata(synthetic)"
			}
			fmt.Fprintf(w, "  op %-14s reg=%s%s off=%#x alloc=%d instrs=%d span=%d src=%s rva=%#x",
				op.Kind, op.Register, reg2(op.Register2), op.Offset,
				op.AllocSize, op.InstrCount, op.ByteSpan, origin, op.SourceRVA)
			if op.Phantom {
				fmt.Fprint(w, " phantom")
			}
			fmt.Fprintln(w)
		}
		if len(q.PendingOps) == 0 {
			if q.Complete {
				fmt.Fprintln(w, "  no unwind operations required: metadata describes a complete leaf frame")
			} else {
				fmt.Fprintln(w, "  EVIDENCE UNTRUSTED: no remaining unwind operations at this PC despite an incomplete frame")
			}
		}
	}
	return nil
}

func reg2(r string) string {
	if r == "" {
		return ""
	}
	return "/" + r
}

// parseARM64ExceptionDirectory populates the existing Exceptions slice for an
// ARM64 image using its 8-byte RUNTIME_FUNCTION layout. The AMD64 path in
// exception.go is untouched. Deep decoding happens lazily/strictly through
// ARM64UnwindView instead.
func (pe *File) parseARM64ExceptionDirectory(rva, size uint32) error {

	fileOffset := pe.GetOffsetFromRva(rva)
	if fileOffset == ^uint32(0) {
		return errARM64Unmapped
	}
	entriesCount := size / arm64RuntimeFunctionSize
	for i := uint32(0); i < entriesCount; i++ {
		offset := fileOffset + i*arm64RuntimeFunctionSize
		b, err := pe.ReadBytesAtOffset(offset, arm64RuntimeFunctionSize)
		if err != nil {
			return err
		}
		begin := binary.LittleEndian.Uint32(b[0:4])
		second := binary.LittleEndian.Uint32(b[4:8])
		flag := second & 3
		entry := ImageRuntimeFunctionEntry{BeginAddress: begin}
		switch flag {
		case 0:
			// Full xdata: the end RVA lives inside the referenced xdata
			// record, which the strict view decodes on demand.
			entry.UnwindInfoAddress = second &^ uint32(3)
		case 1, 2:
			// Packed records carry the function length in words inline.
			entry.EndAddress = begin + ((second>>2)&0x7ff)*arm64InstrSize
		}
		pe.Exceptions = append(pe.Exceptions, Exception{RuntimeFunction: entry})
	}
	if entriesCount > 0 {
		pe.HasException = true
	}
	return nil
}
