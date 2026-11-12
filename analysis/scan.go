package analysis

import (
	"fmt"

	"github.com/tetratelabs/wabin/wasm"
)

// ScanError describes a failure while linearly scanning a function body. The
// Function is the full function index (imported functions precede defined
// ones) and Offset is the byte offset of the offending instruction inside the
// corresponding wasm.Code.Body.
type ScanError struct {
	Function wasm.Index
	Offset   uint32
	Opcode   byte
	Err      error
}

// Error implements error.
func (e *ScanError) Error() string {
	return fmt.Sprintf("analysis: function %d body offset %d opcode 0x%02x: %s",
		e.Function, e.Offset, e.Opcode, e.Err.Error())
}

// Unwrap implements errors.Unwrap.
func (e *ScanError) Unwrap() error {
	return e.Err
}

// tableEffectKind enumerates the way a reachable instruction can modify a
// table summary.
type tableEffectKind byte

const (
	tableEffectSet tableEffectKind = iota
	tableEffectFill
	tableEffectGrow
	tableEffectInit
	tableEffectCopy
)

// tableEffect is a single table-modifying instruction found in a reachable
// function body.
type tableEffect struct {
	kind     tableEffectKind
	offset   uint32
	table    wasm.Index
	srcTable wasm.Index // table.copy only
	segment  int        // table.init only, -1 otherwise
}

// directCall is a statically resolved call instruction.
type directCall struct {
	offset uint32
	target wasm.Index
}

// indirectSite is a call_indirect instruction that has to be (re-)expanded
// against the current table summaries until the fixed point is reached.
type indirectSite struct {
	caller  wasm.Index
	offset  uint32
	table   wasm.Index
	typeIdx wasm.Index
}

// scannedBody is everything the linear scanner extracts from one function.
type scannedBody struct {
	direct  []directCall
	sites   []indirectSite
	effects []tableEffect
}

// scanBody linearly walks a function body without decoding a stack machine.
// It consumes every variable-length immediate so that no instruction is ever
// misread as a call, and reports truncated immediates, out-of-range encodings
// and unsupported proposals as ScanError values.
func scanBody(fn wasm.Index, body []byte) (*scannedBody, error) {
	s := &scanner{fn: fn, body: body, out: &scannedBody{}}
	if err := s.run(); err != nil {
		return nil, err
	}
	return s.out, nil
}

type scanner struct {
	fn   wasm.Index
	body []byte
	pos  int
	out  *scannedBody
}

func (s *scanner) fail(offset int, op byte, format string, args ...interface{}) error {
	return &ScanError{
		Function: s.fn,
		Offset:   uint32(offset),
		Opcode:   op,
		Err:      fmt.Errorf(format, args...),
	}
}

func (s *scanner) readByte(offset int, op byte) (byte, error) {
	if s.pos >= len(s.body) {
		return 0, s.fail(offset, op, "unexpected end of body while reading immediate")
	}
	b := s.body[s.pos]
	s.pos++
	return b, nil
}

// readULEB32 consumes an unsigned LEB128 immediate (at most five bytes) and
// rejects overlong or overflowing encodings.
func (s *scanner) readULEB32(offset int, op byte, what string) (uint32, error) {
	var ret uint32
	for i := 0; i < 5; i++ {
		b, err := s.readByte(offset, op)
		if err != nil {
			return 0, err
		}
		ret |= uint32(b&0x7f) << (7 * i)
		if b&0x80 == 0 {
			if i == 4 && b&0xf0 != 0 {
				return 0, s.fail(offset, op, "%s LEB128 overflows 32 bits", what)
			}
			return ret, nil
		}
	}
	return 0, s.fail(offset, op, "%s LEB128 is longer than five bytes", what)
}

// readSLEB32 consumes a signed LEB128 immediate, as used by i32.const.
func (s *scanner) readSLEB32(offset int, op byte) (int32, error) {
	var ret int32
	var shift uint
	for i := 0; i < 5; i++ {
		b, err := s.readByte(offset, op)
		if err != nil {
			return 0, err
		}
		ret |= int32(b&0x7f) << shift
		shift += 7
		if b&0x80 == 0 {
			if shift < 32 && b&0x40 != 0 {
				ret |= -1 << shift
			}
			return ret, nil
		}
	}
	return 0, s.fail(offset, op, "signed LEB128 is longer than five bytes")
}

// readSLEB64 consumes a signed LEB128 immediate, as used by i64.const.
func (s *scanner) readSLEB64(offset int, op byte) (int64, error) {
	var ret int64
	var shift uint
	for i := 0; i < 10; i++ {
		b, err := s.readByte(offset, op)
		if err != nil {
			return 0, err
		}
		ret |= int64(b&0x7f) << shift
		shift += 7
		if b&0x80 == 0 {
			if shift < 64 && b&0x40 != 0 {
				ret |= -1 << shift
			}
			return ret, nil
		}
	}
	return 0, s.fail(offset, op, "signed LEB128 is longer than ten bytes")
}

func (s *scanner) readFixed(offset int, op byte, n int, what string) error {
	if s.pos+n > len(s.body) {
		return s.fail(offset, op, "unexpected end of body while reading %s", what)
	}
	s.pos += n
	return nil
}

// readBlockType consumes a block type: the empty marker 0x40, a single value
// type byte, or a (positive) signed 33-bit type index encoded as LEB128.
func (s *scanner) readBlockType(offset int, op byte) error {
	b, err := s.readByte(offset, op)
	if err != nil {
		return err
	}
	if b&0x80 == 0 {
		// 0x40 is the empty block type, 0x6b..0x7f are single value types and
		// 0x00..0x3f are single-byte positive type indices.
		return nil
	}
	// Multi-byte LEB128 type index: consume continuation bytes.
	for i := 1; ; i++ {
		if i > 5 {
			return s.fail(offset, op, "block type LEB128 is longer than five bytes")
		}
		b, err = s.readByte(offset, op)
		if err != nil {
			return err
		}
		if b&0x80 == 0 {
			return nil
		}
	}
}

func (s *scanner) run() error {
	depth := 0
	for {
		if s.pos >= len(s.body) {
			return s.fail(len(s.body), 0, "unexpected end of body: missing OpcodeEnd")
		}
		offset := s.pos
		op := s.body[s.pos]
		s.pos++

		switch op {
		case wasm.OpcodeUnreachable, wasm.OpcodeNop, wasm.OpcodeReturn,
			wasm.OpcodeDrop, wasm.OpcodeSelect, wasm.OpcodeRefIsNull:
			// no immediates
		case wasm.OpcodeBlock, wasm.OpcodeLoop, wasm.OpcodeIf:
			if err := s.readBlockType(offset, op); err != nil {
				return err
			}
			depth++
		case wasm.OpcodeElse:
			// no immediates
		case wasm.OpcodeEnd:
			if depth == 0 {
				if s.pos != len(s.body) {
					return s.fail(offset, op, "trailing bytes after function end")
				}
				return nil
			}
			depth--
		case wasm.OpcodeBr, wasm.OpcodeBrIf:
			if _, err := s.readULEB32(offset, op, "label"); err != nil {
				return err
			}
		case wasm.OpcodeBrTable:
			n, err := s.readULEB32(offset, op, "br_table target count")
			if err != nil {
				return err
			}
			for i := uint32(0); i < n+1; i++ { // n targets plus the default label
				if _, err := s.readULEB32(offset, op, "br_table label"); err != nil {
					return err
				}
			}
		case wasm.OpcodeCall:
			target, err := s.readULEB32(offset, op, "function index")
			if err != nil {
				return err
			}
			s.out.direct = append(s.out.direct, directCall{offset: uint32(offset), target: target})
		case wasm.OpcodeCallIndirect:
			typeIdx, err := s.readULEB32(offset, op, "call_indirect type index")
			if err != nil {
				return err
			}
			tableIdx, err := s.readULEB32(offset, op, "call_indirect table index")
			if err != nil {
				return err
			}
			s.out.sites = append(s.out.sites, indirectSite{
				caller: s.fn, offset: uint32(offset), table: tableIdx, typeIdx: typeIdx,
			})
		case wasm.OpcodeLocalGet, wasm.OpcodeLocalSet, wasm.OpcodeLocalTee,
			wasm.OpcodeGlobalGet, wasm.OpcodeGlobalSet:
			if _, err := s.readULEB32(offset, op, "variable index"); err != nil {
				return err
			}
		case wasm.OpcodeTypedSelect:
			n, err := s.readULEB32(offset, op, "select type count")
			if err != nil {
				return err
			}
			for i := uint32(0); i < n; i++ {
				if _, err := s.readByte(offset, op); err != nil {
					return err
				}
			}
		case wasm.OpcodeTableGet:
			if _, err := s.readULEB32(offset, op, "table index"); err != nil {
				return err
			}
		case wasm.OpcodeTableSet:
			table, err := s.readULEB32(offset, op, "table index")
			if err != nil {
				return err
			}
			s.out.effects = append(s.out.effects, tableEffect{
				kind: tableEffectSet, offset: uint32(offset), table: table, segment: -1,
			})
		case wasm.OpcodeI32Load, wasm.OpcodeI64Load, wasm.OpcodeF32Load, wasm.OpcodeF64Load,
			wasm.OpcodeI32Load8S, wasm.OpcodeI32Load8U, wasm.OpcodeI32Load16S, wasm.OpcodeI32Load16U,
			wasm.OpcodeI64Load8S, wasm.OpcodeI64Load8U, wasm.OpcodeI64Load16S, wasm.OpcodeI64Load16U,
			wasm.OpcodeI64Load32S, wasm.OpcodeI64Load32U,
			wasm.OpcodeI32Store, wasm.OpcodeI64Store, wasm.OpcodeF32Store, wasm.OpcodeF64Store,
			wasm.OpcodeI32Store8, wasm.OpcodeI32Store16, wasm.OpcodeI64Store8,
			wasm.OpcodeI64Store16, wasm.OpcodeI64Store32:
			if _, err := s.readULEB32(offset, op, "memory align"); err != nil {
				return err
			}
			if _, err := s.readULEB32(offset, op, "memory offset"); err != nil {
				return err
			}
		case wasm.OpcodeMemorySize, wasm.OpcodeMemoryGrow:
			if _, err := s.readByte(offset, op); err != nil { // memory index, fixed to 0x00 in MVP
				return err
			}
		case wasm.OpcodeI32Const:
			if _, err := s.readSLEB32(offset, op); err != nil {
				return err
			}
		case wasm.OpcodeI64Const:
			if _, err := s.readSLEB64(offset, op); err != nil {
				return err
			}
		case wasm.OpcodeF32Const:
			if err := s.readFixed(offset, op, 4, "f32 constant"); err != nil {
				return err
			}
		case wasm.OpcodeF64Const:
			if err := s.readFixed(offset, op, 8, "f64 constant"); err != nil {
				return err
			}
		case wasm.OpcodeRefNull:
			if _, err := s.readByte(offset, op); err != nil { // reference type
				return err
			}
		case wasm.OpcodeRefFunc:
			// ref.func is not a call: it only materializes a reference.
			if _, err := s.readULEB32(offset, op, "function index"); err != nil {
				return err
			}
		case wasm.OpcodeMiscPrefix:
			if err := s.scanMisc(offset); err != nil {
				return err
			}
		case wasm.OpcodeVecPrefix:
			return s.fail(offset, op, "SIMD (vector) instructions are unsupported by reachability analysis")
		case opcodeThreadsPrefix:
			return s.fail(offset, op, "threads (atomic) instructions are unsupported by reachability analysis")
		case opcodeExceptionHandlingTry, opcodeExceptionHandlingCatch,
			opcodeExceptionHandlingThrow, opcodeExceptionHandlingRethrow,
			opcodeExceptionHandlingThrowRef:
			return s.fail(offset, op, "exception handling instructions are unsupported by reachability analysis")
		default:
			switch {
			case op >= wasm.OpcodeI32Eqz && op <= wasm.OpcodeI64ReinterpretF64: // 0x45..0xbf
				// no immediates: MVP numeric and sign-extension scalar instructions
			case op >= wasm.OpcodeI32Extend8S && op <= wasm.OpcodeI64Extend32S: // 0xc0..0xc4
				// no immediates
			default:
				return s.fail(offset, op, "unsupported or unknown instruction encoding")
			}
		}
	}
}

// Prefix and opcode bytes reserved by proposals the analysis does not model.
const (
	opcodeThreadsPrefix             = 0xfe // atomic memory instructions
	opcodeExceptionHandlingTry      = 0x06
	opcodeExceptionHandlingCatch    = 0x07
	opcodeExceptionHandlingThrow    = 0x08
	opcodeExceptionHandlingRethrow  = 0x09
	opcodeExceptionHandlingThrowRef = 0x0a
)

// scanMisc consumes a 0xfc-prefixed bulk-memory / reference-types instruction.
func (s *scanner) scanMisc(prefixOffset int) error {
	sub32, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "misc sub-opcode")
	if err != nil {
		return err
	}
	sub := wasm.OpcodeMisc(sub32)
	switch sub {
	case wasm.OpcodeMiscI32TruncSatF32S, wasm.OpcodeMiscI32TruncSatF32U,
		wasm.OpcodeMiscI32TruncSatF64S, wasm.OpcodeMiscI32TruncSatF64U,
		wasm.OpcodeMiscI64TruncSatF32S, wasm.OpcodeMiscI64TruncSatF32U,
		wasm.OpcodeMiscI64TruncSatF64S, wasm.OpcodeMiscI64TruncSatF64U:
		// no immediates
	case wasm.OpcodeMiscMemoryInit:
		if _, err = s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "memory.init data index"); err != nil {
			return err
		}
		if _, err = s.readByte(prefixOffset, wasm.OpcodeMiscPrefix); err != nil { // memory index
			return err
		}
	case wasm.OpcodeMiscDataDrop:
		if _, err = s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "data.drop data index"); err != nil {
			return err
		}
	case wasm.OpcodeMiscMemoryCopy:
		if _, err = s.readByte(prefixOffset, wasm.OpcodeMiscPrefix); err != nil { // destination memory
			return err
		}
		if _, err = s.readByte(prefixOffset, wasm.OpcodeMiscPrefix); err != nil { // source memory
			return err
		}
	case wasm.OpcodeMiscMemoryFill:
		if _, err = s.readByte(prefixOffset, wasm.OpcodeMiscPrefix); err != nil { // memory index
			return err
		}
	case wasm.OpcodeMiscTableInit:
		segment, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.init element index")
		if err != nil {
			return err
		}
		table, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.init table index")
		if err != nil {
			return err
		}
		s.out.effects = append(s.out.effects, tableEffect{
			kind: tableEffectInit, offset: uint32(prefixOffset), table: table, segment: int(segment),
		})
	case wasm.OpcodeMiscElemDrop:
		// elem.drop does not shrink the path-insensitive candidate union, but
		// its immediate still has to be consumed correctly.
		if _, err = s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "elem.drop element index"); err != nil {
			return err
		}
	case wasm.OpcodeMiscTableCopy:
		dst, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.copy destination table")
		if err != nil {
			return err
		}
		src, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.copy source table")
		if err != nil {
			return err
		}
		s.out.effects = append(s.out.effects, tableEffect{
			kind: tableEffectCopy, offset: uint32(prefixOffset), table: dst, srcTable: src, segment: -1,
		})
	case wasm.OpcodeMiscTableGrow:
		table, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.grow table index")
		if err != nil {
			return err
		}
		s.out.effects = append(s.out.effects, tableEffect{
			kind: tableEffectGrow, offset: uint32(prefixOffset), table: table, segment: -1,
		})
	case wasm.OpcodeMiscTableSize:
		if _, err = s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.size table index"); err != nil {
			return err
		}
	case wasm.OpcodeMiscTableFill:
		table, err := s.readULEB32(prefixOffset, wasm.OpcodeMiscPrefix, "table.fill table index")
		if err != nil {
			return err
		}
		s.out.effects = append(s.out.effects, tableEffect{
			kind: tableEffectFill, offset: uint32(prefixOffset), table: table, segment: -1,
		})
	default:
		return s.fail(prefixOffset, wasm.OpcodeMiscPrefix,
			"unsupported misc sub-opcode 0x%02x", sub)
	}
	return nil
}
