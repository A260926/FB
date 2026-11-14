package op

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"

	"github.com/go-delve/delve/pkg/dwarf"
	"github.com/go-delve/delve/pkg/dwarf/leb128"
)

// Opcode represent a DWARF stack program instruction.
// See ./opcodes.go for a full list.
type Opcode byte

//go:generate go run ../../../_scripts/gen-opcodes.go opcodes.table opcodes.go

type stackfn func(Opcode, *context) error

type ReadMemoryFunc func([]byte, uint64) (int, error)

// CallContext carries the state needed to evaluate DW_OP_call2, DW_OP_call4
// and DW_OP_call_ref while executing a DWARF location expression through
// ExecuteStackProgramCalls.
type CallContext struct {
	// DIEOffset is the offset of the DIE owning the root expression, it is
	// only used to annotate error messages.
	DIEOffset uint64
	// CUBase is the offset of the start of the compilation unit containing
	// the root expression, the operands of DW_OP_call2 and DW_OP_call4 are
	// relative to it.
	CUBase uint64
	// OffsetSize is the size in bytes of offsets in the compilation unit
	// containing the root expression (4 for DWARF32, 8 for DWARF64).
	OffsetSize int
	// Resolve returns the location expression of the procedure DIE at
	// offset off (an absolute offset into debug_info), the offset of the
	// start of the compilation unit containing it and that unit's offset
	// size. A procedure without a location attribute is legal and must
	// return a nil expression.
	Resolve func(off uint64) (expr []byte, cuBase uint64, offsetSize int, err error)
}

// callFrame is a suspended expression: when a DW_OP_call* opcode is executed
// the current expression is pushed on the frame stack and the called
// expression becomes the current one; when the current expression is
// exhausted the most recent frame is popped and execution resumes after the
// call opcode.
type callFrame struct {
	buf        *bytes.Buffer
	prog       []byte
	cuBase     uint64
	offsetSize int
	dieOff     uint64
}

type context struct {
	buf     *bytes.Buffer
	prog    []byte
	stack   []int64
	pieces  []Piece
	ptrSize int

	DwarfRegisters
	readMemory ReadMemoryFunc

	// call evaluation state, only used by ExecuteStackProgramCalls
	calls      *CallContext
	cuBase     uint64 // base of the CU containing the current expression
	offsetSize int    // offset size of the CU containing the current expression
	dieOff     uint64 // DIE owning the current expression
	frames     []callFrame
	active     map[uint64]bool // DIEs on the active call chain
	budget     int             // remaining execution steps
	curOff     int             // offset of the current opcode in prog
}

// Piece is a piece of memory stored either at an address or in a register.
type Piece struct {
	Size  int
	Kind  PieceKind
	Val   uint64
	Bytes []byte
}

// PieceKind describes the kind of a piece.
type PieceKind uint8

const (
	AddrPiece PieceKind = iota // The piece is stored in memory, Val is the address
	RegPiece                   // The piece is stored in a register, Val is the register number
	ImmPiece                   // The piece is an immediate value, Val or Bytes is the value
)

var (
	ErrStackUnderflow        = errors.New("DWARF stack underflow")
	ErrStackIndexOutOfBounds = errors.New("DWARF stack index out of bounds")
	ErrMemoryReadUnavailable = errors.New("memory read unavailable")
	ErrExecutionLimit        = errors.New("DWARF expression execution limit exceeded")
)

const arbitraryExecutionLimitFactor = 10

// ExecuteStackProgram executes a DWARF location expression and returns
// either an address (int64), or a slice of Pieces for location expressions
// that don't evaluate to an address (such as register and composite expressions).
func ExecuteStackProgram(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc) (int64, []Piece, error) {
	ctxt := &context{
		buf:            bytes.NewBuffer(instructions),
		prog:           instructions,
		stack:          make([]int64, 0, 3),
		DwarfRegisters: regs,
		ptrSize:        ptrSize,
		readMemory:     readMemory,
	}

	if err := ctxt.run(); err != nil {
		return 0, nil, err
	}

	return ctxt.result()
}

// ExecuteStackProgramCalls is like ExecuteStackProgram but DW_OP_call2,
// DW_OP_call4 and DW_OP_call_ref are evaluated by resolving the called
// procedure through calls. The called procedures share the value stack and
// the location pieces of the caller; when a called procedure ends execution
// resumes after the call opcode. Jumps (DW_OP_skip, DW_OP_bra) are relative
// to the expression they appear in and, unlike in ExecuteStackProgram,
// jumping outside of it is an error.
func ExecuteStackProgramCalls(regs DwarfRegisters, instructions []byte, ptrSize int, readMemory ReadMemoryFunc, calls *CallContext) (int64, []Piece, error) {
	ctxt := &context{
		buf:            bytes.NewBuffer(instructions),
		prog:           instructions,
		stack:          make([]int64, 0, 3),
		DwarfRegisters: regs,
		ptrSize:        ptrSize,
		readMemory:     readMemory,
		calls:          calls,
		cuBase:         calls.CUBase,
		offsetSize:     calls.OffsetSize,
		dieOff:         calls.DIEOffset,
		active:         map[uint64]bool{calls.DIEOffset: true},
	}

	if err := ctxt.run(); err != nil {
		return 0, nil, err
	}

	return ctxt.result()
}

// run executes opcodes until the expression (and all its caller frames) are
// exhausted. When the execution budget runs out ExecuteStackProgramCalls
// returns ErrExecutionLimit while ExecuteStackProgram silently stops,
// preserving its historical behavior.
func (ctxt *context) run() error {
	ctxt.budget = len(ctxt.prog) * arbitraryExecutionLimitFactor
	if ctxt.budget == 0 {
		ctxt.budget = 1
	}
	for tick := 0; ; tick++ {
		if tick >= ctxt.budget {
			if ctxt.calls != nil {
				return ctxt.errAt(ErrExecutionLimit)
			}
			return nil
		}
		opcodeByte, err := ctxt.buf.ReadByte()
		if err != nil {
			if !ctxt.popFrame() {
				return nil
			}
			continue
		}
		opcode := Opcode(opcodeByte)
		if opcode == DW_OP_nop {
			continue
		}
		fn, ok := oplut[opcode]
		if !ok {
			if ctxt.calls != nil {
				switch opcode {
				case DW_OP_call2, DW_OP_call4, DW_OP_call_ref:
					fn, ok = call, true
				}
			}
			if !ok {
				return ctxt.errAt(fmt.Errorf("invalid instruction %#v", opcode))
			}
		}
		ctxt.curOff = len(ctxt.prog) - ctxt.buf.Len() - 1

		if err := fn(opcode, ctxt); err != nil {
			return ctxt.errAt(err)
		}
	}
}

// result converts the final state of the context into the return values of
// the stack program entry points.
func (ctxt *context) result() (int64, []Piece, error) {
	if ctxt.pieces != nil {
		if len(ctxt.pieces) == 1 && ctxt.pieces[0].Kind == RegPiece {
			return int64(ctxt.Uint64Val(ctxt.pieces[0].Val)), ctxt.pieces, nil
		}
		return 0, ctxt.pieces, nil
	}

	if len(ctxt.stack) == 0 {
		return 0, nil, errors.New("empty OP stack")
	}

	return ctxt.stack[len(ctxt.stack)-1], nil, nil
}

// errAt annotates err with the DIE owning the current expression and the
// offset of the current opcode inside it. Errors produced through
// ExecuteStackProgram are returned unchanged.
func (ctxt *context) errAt(err error) error {
	if err == nil || ctxt.calls == nil {
		return err
	}
	return fmt.Errorf("DIE %#x: expression offset %#x: %w", ctxt.dieOff, ctxt.curOff, err)
}

// popFrame restores the expression of the caller after a called procedure
// ends. It returns false when there is no caller left.
func (ctxt *context) popFrame() bool {
	if len(ctxt.frames) == 0 {
		return false
	}
	delete(ctxt.active, ctxt.dieOff)
	f := ctxt.frames[len(ctxt.frames)-1]
	ctxt.frames = ctxt.frames[:len(ctxt.frames)-1]
	ctxt.buf, ctxt.prog = f.buf, f.prog
	ctxt.cuBase, ctxt.offsetSize, ctxt.dieOff = f.cuBase, f.offsetSize, f.dieOff
	return true
}

// callProcedure suspends the current expression and starts evaluating the
// procedure at DIE offset off, which shares the value stack and the location
// pieces of its caller.
func (ctxt *context) callProcedure(off uint64) error {
	if ctxt.active[off] {
		return fmt.Errorf("recursive call to procedure at DIE %#x", off)
	}
	expr, cuBase, offsetSize, err := ctxt.calls.Resolve(off)
	if err != nil {
		return err
	}
	ctxt.frames = append(ctxt.frames, callFrame{
		buf:        ctxt.buf,
		prog:       ctxt.prog,
		cuBase:     ctxt.cuBase,
		offsetSize: ctxt.offsetSize,
		dieOff:     ctxt.dieOff,
	})
	ctxt.buf = bytes.NewBuffer(expr)
	ctxt.prog = expr
	ctxt.cuBase = cuBase
	ctxt.offsetSize = offsetSize
	ctxt.dieOff = off
	ctxt.active[off] = true
	ctxt.budget += len(expr) * arbitraryExecutionLimitFactor
	return nil
}

// callerBuf returns the innermost suspended expression that still has
// opcodes to execute, or nil if there is none.
func (ctxt *context) callerBuf() *bytes.Buffer {
	for i := len(ctxt.frames) - 1; i >= 0; i-- {
		if ctxt.frames[i].buf.Len() > 0 {
			return ctxt.frames[i].buf
		}
	}
	return nil
}

// call executes DW_OP_call2, DW_OP_call4 and DW_OP_call_ref.
func call(opcode Opcode, ctxt *context) error {
	var (
		off uint64
		err error
	)
	switch opcode {
	case DW_OP_call2:
		off, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 2)
	case DW_OP_call4:
		off, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 4)
	case DW_OP_call_ref:
		off, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, ctxt.offsetSize)
	default:
		panic("internal error")
	}
	if err != nil {
		return fmt.Errorf("reading %s operand: %w", opcodeName[opcode], err)
	}
	if opcode != DW_OP_call_ref {
		// relative to the start of the compilation unit containing the
		// current expression
		off += ctxt.cuBase
	}
	return ctxt.callProcedure(off)
}

// PrettyPrint prints the DWARF stack program instructions to `out`.
func PrettyPrint(out io.Writer, instructions []byte, regnumToName func(uint64) string) {
	in := bytes.NewBuffer(instructions)

	for {
		opcode, err := in.ReadByte()
		if err != nil {
			break
		}
		op := Opcode(opcode)
		if name, hasname := opcodeName[op]; hasname {
			io.WriteString(out, name)
			if regnumToName != nil {
				if op >= DW_OP_reg0 && op <= DW_OP_reg31 {
					fmt.Fprintf(out, "(%s)", regnumToName(uint64(op-DW_OP_reg0)))
				} else if op >= DW_OP_breg0 && op <= DW_OP_breg31 {
					fmt.Fprintf(out, "(%s)", regnumToName(uint64(op-DW_OP_breg0)))
				}
			}
			out.Write([]byte{' '})
		} else {
			fmt.Fprintf(out, "%#x ", opcode)
		}

		for i, arg := range opcodeArgs[Opcode(opcode)] {
			var regnum uint64
			switch arg {
			case 's':
				n, _ := leb128.DecodeSigned(in)
				regnum = uint64(n)
				fmt.Fprintf(out, "%#x ", n)
			case 'u':
				n, _ := leb128.DecodeUnsigned(in)
				regnum = n
				fmt.Fprintf(out, "%#x ", n)
			case '1':
				var x uint8
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case '2':
				var x uint16
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case '4':
				var x uint32
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case '8':
				var x uint64
				binary.Read(in, binary.LittleEndian, &x)
				fmt.Fprintf(out, "%#x ", x)
			case 'B':
				sz, _ := leb128.DecodeUnsigned(in)
				data := make([]byte, sz)
				sz2, _ := in.Read(data)
				data = data[:sz2]
				fmt.Fprintf(out, "%d [%x] ", sz, data)
			}
			if regnumToName != nil && i == 0 && (op == DW_OP_regx || op == DW_OP_bregx) {
				fmt.Fprintf(out, "(%s)", regnumToName(regnum))
			}
		}
	}
}

// closeLoc is called by opcodes that can only appear at the end of a
// location expression (DW_OP_regN, DW_OP_regx, DW_OP_stack_value...).
// It checks that we are at the end of the program or that the following
// opcode is DW_OP_piece or DW_OP_bit_piece and processes them.
// When the expression of a called procedure is exhausted the location is
// closed by the caller: if the next opcode of the caller is DW_OP_piece it
// ends this location, the call return by itself does not.
func (ctxt *context) closeLoc(opcode0 Opcode, piece Piece) error {
	if ctxt.buf.Len() == 0 {
		caller := ctxt.callerBuf()
		if caller == nil {
			ctxt.pieces = append(ctxt.pieces, piece)
			return nil
		}

		// DWARF doesn't say what happens to the operand stack at the end of
		// a location expression, resetting it here.
		ctxt.stack = ctxt.stack[:0]

		b, err := caller.ReadByte()
		if err != nil {
			return err
		}
		opcode := Opcode(b)

		switch opcode {
		case DW_OP_piece:
			sz, _ := leb128.DecodeUnsigned(caller)
			piece.Size = int(sz)
			ctxt.pieces = append(ctxt.pieces, piece)
			return nil

		case DW_OP_bit_piece:
			// not supported
			return fmt.Errorf("invalid instruction %#v", opcode)
		default:
			return fmt.Errorf("invalid instruction %#v after %#v", opcode, opcode0)
		}
	}

	// DWARF doesn't say what happens to the operand stack at the end of a
	// location expression, resetting it here.
	ctxt.stack = ctxt.stack[:0]

	b, err := ctxt.buf.ReadByte()
	if err != nil {
		return err
	}
	opcode := Opcode(b)

	switch opcode {
	case DW_OP_piece:
		sz, _ := leb128.DecodeUnsigned(ctxt.buf)
		piece.Size = int(sz)
		ctxt.pieces = append(ctxt.pieces, piece)
		return nil

	case DW_OP_bit_piece:
		// not supported
		return fmt.Errorf("invalid instruction %#v", opcode)
	default:
		return fmt.Errorf("invalid instruction %#v after %#v", opcode, opcode0)
	}
}

func callframecfa(opcode Opcode, ctxt *context) error {
	if ctxt.CFA == 0 {
		return errors.New("could not retrieve CFA for current PC")
	}
	ctxt.stack = append(ctxt.stack, ctxt.CFA)
	return nil
}

func addr(opcode Opcode, ctxt *context) error {
	buf := ctxt.buf.Next(ctxt.ptrSize)
	stack, err := dwarf.ReadUintRaw(bytes.NewReader(buf), binary.LittleEndian, ctxt.ptrSize)
	if err != nil {
		return err
	}
	ctxt.stack = append(ctxt.stack, int64(stack+ctxt.StaticBase))
	return nil
}

func plusuconsts(opcode Opcode, ctxt *context) error {
	slen := len(ctxt.stack)
	if slen < 1 {
		return ErrStackUnderflow
	}
	num, _ := leb128.DecodeUnsigned(ctxt.buf)
	ctxt.stack[slen-1] = ctxt.stack[slen-1] + int64(num)
	return nil
}

func consts(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeSigned(ctxt.buf)
	ctxt.stack = append(ctxt.stack, num)
	return nil
}

func framebase(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeSigned(ctxt.buf)
	ctxt.stack = append(ctxt.stack, ctxt.FrameBase+num)
	return nil
}

func register(opcode Opcode, ctxt *context) error {
	var regnum uint64
	if opcode == DW_OP_regx {
		regnum, _ = leb128.DecodeUnsigned(ctxt.buf)
	} else {
		regnum = uint64(opcode - DW_OP_reg0)
	}
	return ctxt.closeLoc(opcode, Piece{Kind: RegPiece, Val: regnum})
}

func bregister(opcode Opcode, ctxt *context) error {
	var regnum uint64
	if opcode == DW_OP_bregx {
		regnum, _ = leb128.DecodeUnsigned(ctxt.buf)
	} else {
		regnum = uint64(opcode - DW_OP_breg0)
	}
	offset, _ := leb128.DecodeSigned(ctxt.buf)
	if ctxt.Reg(regnum) == nil {
		return fmt.Errorf("register %d not available", regnum)
	}
	ctxt.stack = append(ctxt.stack, int64(ctxt.Uint64Val(regnum))+offset)
	return nil
}

func piece(opcode Opcode, ctxt *context) error {
	sz, _ := leb128.DecodeUnsigned(ctxt.buf)

	if len(ctxt.stack) == 0 {
		// nothing on the stack means this piece is unavailable (padding,
		// optimized away...), see DWARFv4 sec. 2.6.1.3 page 30.
		ctxt.pieces = append(ctxt.pieces, Piece{Size: int(sz), Kind: ImmPiece, Val: 0})
		return nil
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	ctxt.pieces = append(ctxt.pieces, Piece{Size: int(sz), Kind: AddrPiece, Val: uint64(addr)})
	ctxt.stack = ctxt.stack[:0]
	return nil
}

func literal(opcode Opcode, ctxt *context) error {
	ctxt.stack = append(ctxt.stack, int64(opcode-DW_OP_lit0))
	return nil
}

func constnu(opcode Opcode, ctxt *context) error {
	var (
		n   uint64
		err error
	)
	switch opcode {
	case DW_OP_const1u:
		var b uint8
		b, err = ctxt.buf.ReadByte()
		n = uint64(b)
	case DW_OP_const2u:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 2)
	case DW_OP_const4u:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 4)
	case DW_OP_const8u:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 8)
	default:
		panic("internal error")
	}
	if err != nil {
		return err
	}
	ctxt.stack = append(ctxt.stack, int64(n))
	return nil
}

func constns(opcode Opcode, ctxt *context) error {
	var (
		n   uint64
		err error
	)
	switch opcode {
	case DW_OP_const1s:
		var b uint8
		b, err = ctxt.buf.ReadByte()
		n = uint64(int64(int8(b)))
	case DW_OP_const2s:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 2)
		n = uint64(int64(int16(n)))
	case DW_OP_const4s:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 4)
		n = uint64(int64(int32(n)))
	case DW_OP_const8s:
		n, err = dwarf.ReadUintRaw(ctxt.buf, binary.LittleEndian, 8)
	default:
		panic("internal error")
	}
	if err != nil {
		return err
	}
	ctxt.stack = append(ctxt.stack, int64(n))
	return nil
}

func constu(opcode Opcode, ctxt *context) error {
	num, _ := leb128.DecodeUnsigned(ctxt.buf)
	ctxt.stack = append(ctxt.stack, int64(num))
	return nil
}

func dup(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}
	ctxt.stack = append(ctxt.stack, ctxt.stack[len(ctxt.stack)-1])
	return nil
}

func drop(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	return nil
}

func pick(opcode Opcode, ctxt *context) error {
	var n byte
	switch opcode {
	case DW_OP_pick:
		n, _ = ctxt.buf.ReadByte()
	case DW_OP_over:
		n = 1
	default:
		panic("internal error")
	}
	idx := len(ctxt.stack) - 1 - int(n)
	if idx < 0 || idx >= len(ctxt.stack) {
		return ErrStackIndexOutOfBounds
	}
	ctxt.stack = append(ctxt.stack, ctxt.stack[idx])
	return nil
}

func swap(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 2 {
		return ErrStackUnderflow
	}
	ctxt.stack[len(ctxt.stack)-1], ctxt.stack[len(ctxt.stack)-2] = ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-1]
	return nil
}

func rot(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 3 {
		return ErrStackUnderflow
	}
	ctxt.stack[len(ctxt.stack)-1], ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-3] = ctxt.stack[len(ctxt.stack)-2], ctxt.stack[len(ctxt.stack)-3], ctxt.stack[len(ctxt.stack)-1]
	return nil
}

func unaryop(opcode Opcode, ctxt *context) error {
	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	operand := ctxt.stack[len(ctxt.stack)-1]
	switch opcode {
	case DW_OP_abs:
		if operand < 0 {
			operand = -operand
		}
	case DW_OP_neg:
		operand = -operand
	case DW_OP_not:
		operand = ^operand
	default:
		panic("internal error")
	}
	ctxt.stack[len(ctxt.stack)-1] = operand
	return nil
}

func binaryop(opcode Opcode, ctxt *context) error {
	if len(ctxt.stack) < 2 {
		return ErrStackUnderflow
	}
	second := ctxt.stack[len(ctxt.stack)-2]
	top := ctxt.stack[len(ctxt.stack)-1]
	var r int64
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-2]
	switch opcode {
	case DW_OP_and:
		r = second & top
	case DW_OP_div:
		r = second / top
	case DW_OP_minus:
		r = second - top
	case DW_OP_mod:
		r = second % top
	case DW_OP_mul:
		r = second * top
	case DW_OP_or:
		r = second | top
	case DW_OP_plus:
		r = second + top
	case DW_OP_shl:
		r = second << uint64(top)
	case DW_OP_shr:
		r = int64(uint64(second) >> uint64(top))
	case DW_OP_shra:
		r = second >> uint64(top)
	case DW_OP_xor:
		r = second ^ top
	case DW_OP_le:
		r = bool2int(second <= top)
	case DW_OP_ge:
		r = bool2int(second >= top)
	case DW_OP_eq:
		r = bool2int(second == top)
	case DW_OP_lt:
		r = bool2int(second < top)
	case DW_OP_gt:
		r = bool2int(second > top)
	case DW_OP_ne:
		r = bool2int(second != top)
	default:
		panic("internal error")
	}
	ctxt.stack = append(ctxt.stack, r)
	return nil
}

func bool2int(b bool) int64 {
	if b {
		return 1
	}
	return 0
}

func (ctxt *context) jump(n int16) error {
	i := len(ctxt.prog) - ctxt.buf.Len() + int(n)
	if ctxt.calls != nil {
		// jump coordinates belong to the current expression, jumping
		// outside of it is an error
		if i < 0 || i > len(ctxt.prog) {
			return fmt.Errorf("branch target %d out of bounds for expression of %d bytes", i, len(ctxt.prog))
		}
	} else {
		if i < 0 {
			return ErrStackUnderflow
		}
		if i >= len(ctxt.prog) {
			i = len(ctxt.prog)
		}
	}
	ctxt.buf = bytes.NewBuffer(ctxt.prog[i:])
	return nil
}

func skip(_ Opcode, ctxt *context) error {
	var n int16
	binary.Read(ctxt.buf, binary.LittleEndian, &n)
	return ctxt.jump(n)
}

func bra(_ Opcode, ctxt *context) error {
	var n int16
	binary.Read(ctxt.buf, binary.LittleEndian, &n)

	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	top := ctxt.stack[len(ctxt.stack)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	if top != 0 {
		return ctxt.jump(n)
	}
	return nil
}

func stackvalue(_ Opcode, ctxt *context) error {
	if len(ctxt.stack) < 1 {
		return ErrStackUnderflow
	}
	val := ctxt.stack[len(ctxt.stack)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	return ctxt.closeLoc(DW_OP_stack_value, Piece{Kind: ImmPiece, Val: uint64(val)})
}

func implicitvalue(_ Opcode, ctxt *context) error {
	sz, _ := leb128.DecodeUnsigned(ctxt.buf)
	block := make([]byte, sz)
	n, _ := ctxt.buf.Read(block)
	if uint64(n) != sz {
		return fmt.Errorf("insufficient bytes read while reading DW_OP_implicit_value's block %d (expected: %d)", n, sz)
	}
	return ctxt.closeLoc(DW_OP_implicit_value, Piece{Kind: ImmPiece, Bytes: block, Size: int(sz)})
}

func deref(op Opcode, ctxt *context) error {
	if ctxt.readMemory == nil {
		return ErrMemoryReadUnavailable
	}

	sz := ctxt.ptrSize
	if op == DW_OP_deref_size || op == DW_OP_xderef_size {
		n, err := ctxt.buf.ReadByte()
		if err != nil {
			return err
		}
		sz = int(n)
	}

	if len(ctxt.stack) == 0 {
		return ErrStackUnderflow
	}

	addr := ctxt.stack[len(ctxt.stack)-1]
	ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]

	if op == DW_OP_xderef || op == DW_OP_xderef_size {
		if len(ctxt.stack) == 0 {
			return ErrStackUnderflow
		}
		// the second element on the stack is the "address space identifier" which we don't do anything with
		ctxt.stack = ctxt.stack[:len(ctxt.stack)-1]
	}

	buf := make([]byte, sz)
	_, err := ctxt.readMemory(buf, uint64(addr))
	if err != nil {
		return err
	}

	x, err := dwarf.ReadUintRaw(bytes.NewReader(buf), binary.LittleEndian, sz)
	if err != nil {
		return err
	}

	ctxt.stack = append(ctxt.stack, int64(x))

	return nil
}
