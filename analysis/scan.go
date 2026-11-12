package analysis

import (
	"bytes"
	"errors"
	"fmt"
	"io"

	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// scanLimits are the index-space sizes needed to reject out-of-range
// immediates while scanning a function body.
type scanLimits struct {
	funcCount    uint32
	tableCount   uint32
	typeCount    uint32
	globalCount  uint32
	memoryCount  uint32
	dataCount    uint32
	elementCount uint32
	localCount   uint32
}

type directCall struct {
	offset int
	target wasm.Index
}

type indirectCall struct {
	caller  wasm.Index
	offset  int
	table   wasm.Index
	typeIdx wasm.Index
}

type tableActionKind byte

const (
	tableActionSet tableActionKind = iota
	tableActionFill
	tableActionGrow
	tableActionInit
	tableActionCopy
	tableActionDrop
)

type tableAction struct {
	kind    tableActionKind
	offset  int
	table   wasm.Index
	source  wasm.Index
	segment wasm.Index
}

type scanResult struct {
	direct   []directCall
	indirect []indirectCall
	actions  []tableAction
}

type scanner struct {
	body     []byte
	fn       wasm.Index
	pos      int
	depth    int
	finished bool
	limits   scanLimits
	result   scanResult
}

// scanFunctionBody scans one function body. It consumes every immediate
// according to its binary encoding. Unsupported proposal encodings are
// rejected rather than blindly scanned, so a stray byte is never mistaken for
// a call opcode.
func scanFunctionBody(body []byte, fn wasm.Index, limits scanLimits) (*scanResult, *Error) {
	s := &scanner{body: body, fn: fn, depth: 1, limits: limits}
	for !s.finished {
		if s.pos >= len(body) {
			return nil, s.errAt(len(body), "unexpected end of body: missing end opcode")
		}
		instOff := s.pos
		op := body[s.pos]
		s.pos++
		if err := s.step(op, instOff); err != nil {
			return nil, err
		}
	}
	if s.pos != len(body) {
		return nil, s.errAt(s.pos, "trailing bytes after function body end")
	}
	return &s.result, nil
}

func (s *scanner) errAt(offset int, format string, args ...interface{}) *Error {
	return &Error{Function: s.fn, IsFunction: true, Offset: offset, Segment: -1, Message: fmt.Sprintf(format, args...)}
}

func immediateErr(name string, err error) string {
	if errors.Is(err, io.EOF) || errors.Is(err, io.ErrUnexpectedEOF) {
		return "truncated " + name + " immediate"
	}
	return "invalid " + name + " immediate: " + err.Error()
}

func (s *scanner) readU32(name string, instOff int) (uint32, *Error) {
	v, n, err := leb128.DecodeUint32(bytes.NewReader(s.body[s.pos:]))
	if err != nil {
		return 0, s.errAt(instOff, "%s", immediateErr(name, err))
	}
	s.pos += int(n)
	return v, nil
}

func (s *scanner) readS32(name string, instOff int) *Error {
	_, n, err := leb128.DecodeInt32(bytes.NewReader(s.body[s.pos:]))
	if err != nil {
		return s.errAt(instOff, "%s", immediateErr(name, err))
	}
	s.pos += int(n)
	return nil
}

func (s *scanner) readS64(name string, instOff int) *Error {
	_, n, err := leb128.DecodeInt64(bytes.NewReader(s.body[s.pos:]))
	if err != nil {
		return s.errAt(instOff, "%s", immediateErr(name, err))
	}
	s.pos += int(n)
	return nil
}

func (s *scanner) readByte(name string, instOff int) (byte, *Error) {
	if s.pos >= len(s.body) {
		return 0, s.errAt(instOff, "truncated %s immediate", name)
	}
	b := s.body[s.pos]
	s.pos++
	return b, nil
}

func (s *scanner) skip(n int, name string, instOff int) *Error {
	if len(s.body)-s.pos < n {
		s.pos = len(s.body)
		return s.errAt(instOff, "truncated %s immediate", name)
	}
	s.pos += n
	return nil
}

func (s *scanner) checkFunc(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.funcCount {
		return s.errAt(instOff, "%s index %d out of range (have %d functions)", what, idx, s.limits.funcCount)
	}
	return nil
}

func (s *scanner) checkTable(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.tableCount {
		return s.errAt(instOff, "%s index %d out of range (have %d tables)", what, idx, s.limits.tableCount)
	}
	return nil
}

func (s *scanner) checkType(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.typeCount {
		return s.errAt(instOff, "%s index %d out of range (have %d types)", what, idx, s.limits.typeCount)
	}
	return nil
}

func (s *scanner) checkGlobal(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.globalCount {
		return s.errAt(instOff, "%s index %d out of range (have %d globals)", what, idx, s.limits.globalCount)
	}
	return nil
}

func (s *scanner) checkMemory(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.memoryCount {
		return s.errAt(instOff, "%s index %d out of range (have %d memories)", what, idx, s.limits.memoryCount)
	}
	return nil
}

func (s *scanner) checkData(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.dataCount {
		return s.errAt(instOff, "%s index %d out of range (have %d data segments)", what, idx, s.limits.dataCount)
	}
	return nil
}

func (s *scanner) checkElement(idx uint32, instOff int, what string) *Error {
	if idx >= s.limits.elementCount {
		return s.errAt(instOff, "%s index %d out of range (have %d element segments)", what, idx, s.limits.elementCount)
	}
	return nil
}

func isValueType(b byte) bool {
	switch b {
	case wasm.ValueTypeI32, wasm.ValueTypeI64, wasm.ValueTypeF32, wasm.ValueTypeF64,
		wasm.ValueTypeFuncref, wasm.ValueTypeExternref, wasm.ValueTypeV128:
		return true
	}
	return false
}

func (s *scanner) readValueType(name string, instOff int) *Error {
	b, err := s.readByte(name, instOff)
	if err != nil {
		return err
	}
	if !isValueType(b) {
		return s.errAt(instOff, "invalid %s value type 0x%x", name, b)
	}
	if b == wasm.ValueTypeV128 {
		return s.errAt(instOff, "SIMD value type v128 is unsupported")
	}
	return nil
}

func (s *scanner) readBlockType(instOff int) *Error {
	if s.pos >= len(s.body) {
		return s.errAt(instOff, "truncated block type immediate")
	}
	b := s.body[s.pos]
	if b == 0x40 { // empty block type
		s.pos++
		return nil
	}
	if isValueType(b) {
		return s.readValueType("block type", instOff)
	}
	// Otherwise a signed 33-bit type index into the type section.
	v, n, err := leb128.DecodeInt33AsInt64(bytes.NewReader(s.body[s.pos:]))
	if err != nil {
		return s.errAt(instOff, "%s", immediateErr("block type", err))
	}
	if v < 0 {
		return s.errAt(instOff, "unsupported block type 0x%x", b)
	}
	if uint32(v) >= s.limits.typeCount {
		return s.errAt(instOff, "block type index %d out of range (have %d types)", v, s.limits.typeCount)
	}
	s.pos += int(n)
	return nil
}

func (s *scanner) checkLabel(label uint32, instOff int, what string) *Error {
	if label >= uint32(s.depth) {
		return s.errAt(instOff, "%s target %d out of range (depth %d)", what, label, s.depth)
	}
	return nil
}

func (s *scanner) step(op byte, instOff int) *Error {
	switch op {
	// Control instructions without immediates.
	case wasm.OpcodeUnreachable, wasm.OpcodeNop, wasm.OpcodeElse, wasm.OpcodeReturn,
		wasm.OpcodeDrop, wasm.OpcodeSelect, wasm.OpcodeRefIsNull:
		return nil

	case wasm.OpcodeBlock, wasm.OpcodeLoop, wasm.OpcodeIf:
		if err := s.readBlockType(instOff); err != nil {
			return err
		}
		s.depth++
		return nil

	case wasm.OpcodeEnd:
		s.depth--
		if s.depth == 0 {
			s.finished = true
			return nil
		}
		if s.depth < 0 {
			return s.errAt(instOff, "unexpected end opcode")
		}
		return nil

	case wasm.OpcodeBr, wasm.OpcodeBrIf:
		label, err := s.readU32("branch target", instOff)
		if err != nil {
			return err
		}
		return s.checkLabel(label, instOff, "branch")

	case wasm.OpcodeBrTable:
		count, err := s.readU32("br_table target count", instOff)
		if err != nil {
			return err
		}
		// count branch targets followed by the default target.
		for i := uint32(0); i <= count; i++ {
			label, err := s.readU32("br_table target", instOff)
			if err != nil {
				return err
			}
			if err := s.checkLabel(label, instOff, "br_table"); err != nil {
				return err
			}
		}
		return nil

	case wasm.OpcodeCall:
		idx, err := s.readU32("call", instOff)
		if err != nil {
			return err
		}
		if err := s.checkFunc(idx, instOff, "call"); err != nil {
			return err
		}
		s.result.direct = append(s.result.direct, directCall{offset: instOff, target: idx})
		return nil

	case wasm.OpcodeCallIndirect:
		typeIdx, err := s.readU32("call_indirect type", instOff)
		if err != nil {
			return err
		}
		if err := s.checkType(typeIdx, instOff, "call_indirect type"); err != nil {
			return err
		}
		tableIdx, err := s.readU32("call_indirect table", instOff)
		if err != nil {
			return err
		}
		if err := s.checkTable(tableIdx, instOff, "call_indirect table"); err != nil {
			return err
		}
		s.result.indirect = append(s.result.indirect, indirectCall{
			caller: s.fn, offset: instOff, table: tableIdx, typeIdx: typeIdx,
		})
		return nil

	case wasm.OpcodeTypedSelect:
		count, err := s.readU32("typed select type count", instOff)
		if err != nil {
			return err
		}
		for i := uint32(0); i < count; i++ {
			if err := s.readValueType("typed select", instOff); err != nil {
				return err
			}
		}
		return nil

	case wasm.OpcodeLocalGet, wasm.OpcodeLocalSet, wasm.OpcodeLocalTee:
		idx, err := s.readU32("local", instOff)
		if err != nil {
			return err
		}
		if idx >= s.limits.localCount {
			return s.errAt(instOff, "local index %d out of range (have %d locals)", idx, s.limits.localCount)
		}
		return nil

	case wasm.OpcodeGlobalGet, wasm.OpcodeGlobalSet:
		idx, err := s.readU32("global", instOff)
		if err != nil {
			return err
		}
		return s.checkGlobal(idx, instOff, "global")

	case wasm.OpcodeTableGet:
		idx, err := s.readU32("table.get", instOff)
		if err != nil {
			return err
		}
		return s.checkTable(idx, instOff, "table.get")

	case wasm.OpcodeTableSet:
		idx, err := s.readU32("table.set", instOff)
		if err != nil {
			return err
		}
		if err = s.checkTable(idx, instOff, "table.set"); err != nil {
			return err
		}
		s.result.actions = append(s.result.actions, tableAction{kind: tableActionSet, offset: instOff, table: idx})
		return nil

	case wasm.OpcodeMemorySize, wasm.OpcodeMemoryGrow:
		idx, err := s.readU32("memory", instOff)
		if err != nil {
			return err
		}
		return s.checkMemory(idx, instOff, "memory")

	case wasm.OpcodeI32Const:
		return s.readS32("i32.const", instOff)
	case wasm.OpcodeI64Const:
		return s.readS64("i64.const", instOff)
	case wasm.OpcodeF32Const:
		return s.skip(4, "f32.const", instOff)
	case wasm.OpcodeF64Const:
		return s.skip(8, "f64.const", instOff)

	case wasm.OpcodeRefNull:
		rt, err := s.readByte("ref.null type", instOff)
		if err != nil {
			return err
		}
		if rt != wasm.RefTypeFuncref && rt != wasm.RefTypeExternref {
			return s.errAt(instOff, "invalid ref.null type 0x%x", rt)
		}
		return nil

	case wasm.OpcodeRefFunc:
		idx, err := s.readU32("ref.func", instOff)
		if err != nil {
			return err
		}
		// ref.func is a declaration, not a call. Its index is still validated.
		return s.checkFunc(idx, instOff, "ref.func")

	case wasm.OpcodeMiscPrefix:
		return s.stepMisc(instOff)

	// Explicitly unsupported proposal encodings. These must be rejected before
	// their immediates are (mis)interpreted as further instructions.
	case wasm.OpcodeVecPrefix:
		return s.errAt(instOff, "SIMD opcode stream is unsupported")
	case 0xfe:
		return s.errAt(instOff, "threads/atomic opcode stream is unsupported")
	case 0x06, 0x07, 0x08, 0x09, 0x0a, 0x18, 0x19, 0x1f:
		return s.errAt(instOff, "exception-handling opcode 0x%x is unsupported", op)
	case 0x12, 0x13:
		return s.errAt(instOff, "tail-call opcode 0x%x is unsupported", op)
	}

	// Memory load/store instructions all take a memarg.
	if op >= wasm.OpcodeI32Load && op <= wasm.OpcodeI64Store32 {
		align, err := s.readU32("memory alignment", instOff)
		if err != nil {
			return err
		}
		if align&0x40 != 0 {
			return s.errAt(instOff, "multi-memory memarg encoding is unsupported")
		}
		if _, err = s.readU32("memory offset", instOff); err != nil {
			return err
		}
		return nil
	}

	// All remaining MVP numeric instructions have no immediates.
	if op >= wasm.OpcodeI32Eqz && op <= wasm.OpcodeI64Extend32S {
		return nil
	}

	return s.errAt(instOff, "unsupported opcode 0x%x", op)
}

func (s *scanner) stepMisc(instOff int) *Error {
	sub, err := s.readU32("misc opcode", instOff)
	if err != nil {
		return err
	}
	if sub > 0xff {
		return s.errAt(instOff, "unsupported misc opcode 0x%x", sub)
	}
	switch wasm.OpcodeMisc(sub) {
	case wasm.OpcodeMiscI32TruncSatF32S, wasm.OpcodeMiscI32TruncSatF32U,
		wasm.OpcodeMiscI32TruncSatF64S, wasm.OpcodeMiscI32TruncSatF64U,
		wasm.OpcodeMiscI64TruncSatF32S, wasm.OpcodeMiscI64TruncSatF32U,
		wasm.OpcodeMiscI64TruncSatF64S, wasm.OpcodeMiscI64TruncSatF64U:
		return nil

	case wasm.OpcodeMiscMemoryInit:
		di, err := s.readU32("memory.init data", instOff)
		if err != nil {
			return err
		}
		if err = s.checkData(di, instOff, "memory.init data"); err != nil {
			return err
		}
		mi, err := s.readU32("memory.init memory", instOff)
		if err != nil {
			return err
		}
		return s.checkMemory(mi, instOff, "memory.init memory")

	case wasm.OpcodeMiscDataDrop:
		di, err := s.readU32("data.drop", instOff)
		if err != nil {
			return err
		}
		return s.checkData(di, instOff, "data.drop")

	case wasm.OpcodeMiscMemoryCopy:
		mi, err := s.readU32("memory.copy destination", instOff)
		if err != nil {
			return err
		}
		if err = s.checkMemory(mi, instOff, "memory.copy destination"); err != nil {
			return err
		}
		mi, err = s.readU32("memory.copy source", instOff)
		if err != nil {
			return err
		}
		return s.checkMemory(mi, instOff, "memory.copy source")

	case wasm.OpcodeMiscMemoryFill:
		mi, err := s.readU32("memory.fill", instOff)
		if err != nil {
			return err
		}
		return s.checkMemory(mi, instOff, "memory.fill")

	case wasm.OpcodeMiscTableInit:
		ei, err := s.readU32("table.init element", instOff)
		if err != nil {
			return err
		}
		if err = s.checkElement(ei, instOff, "table.init element"); err != nil {
			return err
		}
		ti, err := s.readU32("table.init table", instOff)
		if err != nil {
			return err
		}
		if err = s.checkTable(ti, instOff, "table.init table"); err != nil {
			return err
		}
		s.result.actions = append(s.result.actions, tableAction{kind: tableActionInit, offset: instOff, table: ti, segment: ei})
		return nil

	case wasm.OpcodeMiscElemDrop:
		ei, err := s.readU32("elem.drop element", instOff)
		if err != nil {
			return err
		}
		if err = s.checkElement(ei, instOff, "elem.drop element"); err != nil {
			return err
		}
		s.result.actions = append(s.result.actions, tableAction{kind: tableActionDrop, offset: instOff, segment: ei})
		return nil

	case wasm.OpcodeMiscTableCopy:
		dst, err := s.readU32("table.copy destination", instOff)
		if err != nil {
			return err
		}
		if err = s.checkTable(dst, instOff, "table.copy destination"); err != nil {
			return err
		}
		src, err := s.readU32("table.copy source", instOff)
		if err != nil {
			return err
		}
		if err = s.checkTable(src, instOff, "table.copy source"); err != nil {
			return err
		}
		s.result.actions = append(s.result.actions, tableAction{kind: tableActionCopy, offset: instOff, table: dst, source: src})
		return nil

	case wasm.OpcodeMiscTableGrow:
		ti, err := s.readU32("table.grow", instOff)
		if err != nil {
			return err
		}
		if err = s.checkTable(ti, instOff, "table.grow"); err != nil {
			return err
		}
		s.result.actions = append(s.result.actions, tableAction{kind: tableActionGrow, offset: instOff, table: ti})
		return nil

	case wasm.OpcodeMiscTableSize:
		ti, err := s.readU32("table.size", instOff)
		if err != nil {
			return err
		}
		return s.checkTable(ti, instOff, "table.size")

	case wasm.OpcodeMiscTableFill:
		ti, err := s.readU32("table.fill", instOff)
		if err != nil {
			return err
		}
		if err = s.checkTable(ti, instOff, "table.fill"); err != nil {
			return err
		}
		s.result.actions = append(s.result.actions, tableAction{kind: tableActionFill, offset: instOff, table: ti})
		return nil
	}
	return s.errAt(instOff, "unsupported misc opcode 0x%x", sub)
}
