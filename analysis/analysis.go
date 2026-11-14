// Package analysis provides static stack-transfer analysis over a single
// WebAssembly function decoded with github.com/tetratelabs/wabin.
//
// AnalyzeStackTransfers reads only the type, import and code sections of a
// module: it never instantiates the module and never executes an import.
// The result is a graph of independently owned value nodes, per-instruction
// inputs/outputs and control edges that describes exactly which stack values
// each structured branch carries to its target.
package analysis

import (
	"bytes"
	"fmt"
	"sort"

	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// SyntheticOffset is the Body offset used by value nodes and edge targets
// that do not belong to any instruction: the synthetic function exit. It
// cannot collide with a real Body offset.
const SyntheticOffset = ^uint32(0)

// ValueKind classifies the origin of a Value node.
type ValueKind int

const (
	// ValueBlockParam is a block/loop/if input parameter, live at the block
	// header. Its identity is fixed when the block is entered, so loop back
	// edges can reference it without unrolling.
	ValueBlockParam ValueKind = iota
	// ValueBlockResult is the merge parameter receiving a block's results at
	// its end. It is kept even when the merge has a single incoming edge.
	ValueBlockResult
	// ValueFuncResult is a result parameter of the synthetic function exit,
	// the target of return and of branches to the outermost label.
	ValueFuncResult
	// ValueInstr is a value produced by an ordinary instruction, identified
	// by Body offset and result index. Constants with equal values are never
	// merged: each instruction owns distinct nodes.
	ValueInstr
	// ValueLocalGet is produced by local.get: an external value source at
	// that position. No cross-block dataflow of mutable locals is tracked.
	ValueLocalGet
)

// Value is a node in the stack-transfer graph. Identity is the triple
// (Kind, Offset, Index), unique within one analysis result.
type Value struct {
	Kind   ValueKind
	Offset uint32 // Body offset of the defining instruction or block; SyntheticOffset for function-level nodes.
	Index  uint32 // Result or parameter position, in declared order.
	Type   wasm.ValueType
}

// String returns a compact debug representation of the value node.
func (v *Value) String() string {
	var kind string
	switch v.Kind {
	case ValueBlockParam:
		kind = "blockparam"
	case ValueBlockResult:
		kind = "blockresult"
	case ValueFuncResult:
		kind = "funcresult"
	case ValueInstr:
		kind = "instr"
	case ValueLocalGet:
		kind = "localget"
	}
	return fmt.Sprintf("%s@%d#%d:%s", kind, v.Offset, v.Index, wasm.ValueTypeName(v.Type))
}

// Instruction records the values one reachable instruction consumes and
// produces. Instructions in dead code have no record: no reachable edges or
// value sources are fabricated for them.
type Instruction struct {
	Offset uint32
	Opcode wasm.Opcode
	// Inputs are the consumed values in stack order: Inputs[len(Inputs)-1]
	// was the top of the stack. For call this is the argument list in
	// declared order.
	Inputs []*Value
	// Outputs are the produced values in stack order.
	Outputs []*Value
	// CallTarget is the callee function index. It is only meaningful when
	// Opcode == wasm.OpcodeCall.
	CallTarget wasm.Index
}

// EdgeKind classifies a control transfer.
type EdgeKind int

const (
	// EdgeEnter enters a block, loop or if-then arm, targeting block params.
	EdgeEnter EdgeKind = iota
	// EdgeEnterElse enters the else arm of an if, from the same input stack
	// as the then arm, targeting block params.
	EdgeEnterElse
	// EdgeFalse is the false edge of an if without else: the inputs flow
	// directly to the block results.
	EdgeFalse
	// EdgeEnd is the fallthrough at a block or function end, targeting the
	// block results or the synthetic function exit.
	EdgeEnd
	// EdgeBranch is an unconditional br. To a loop it targets the loop
	// params (the header), otherwise the target's results.
	EdgeBranch
	// EdgeBranchIf is the taken side of br_if. The condition is consumed
	// first; the fallthrough keeps the arguments, so they are not consumed.
	EdgeBranchIf
	// EdgeBranchTable is one selector of br_table. Selector is the position
	// in the target vector; the default uses Selector == len(targets).
	// Duplicate targets keep distinct selectors.
	EdgeBranchTable
	// EdgeReturn is a return to the synthetic function exit.
	EdgeReturn
)

// EdgeTargetKind identifies which parameter nodes receive an edge's Args.
type EdgeTargetKind int

const (
	// TargetBlockParam targets the input parameters of a block header.
	TargetBlockParam EdgeTargetKind = iota
	// TargetBlockResult targets the merge result parameters of a block end.
	TargetBlockResult
	// TargetFuncResult targets the synthetic function exit.
	TargetFuncResult
)

// Edge is a control transfer carrying stack values to a target's parameter
// nodes. Args are in declared order and parallel to Targets.
type Edge struct {
	FromOffset uint32 // Body offset of the instruction causing the transfer.
	Kind       EdgeKind
	Selector   uint32 // br_table selector index; zero for other kinds.
	TargetKind EdgeTargetKind
	// TargetOffset is the offset of the target block's opcode, or
	// SyntheticOffset for the function exit.
	TargetOffset uint32
	Args         []*Value // Values handed over, in declared order.
	Targets      []*Value // Receiving parameter nodes, parallel to Args.
}

// Graph is the result of AnalyzeStackTransfers. It owns its nodes: nothing
// aliases the analyzed module, which is never modified.
type Graph struct {
	FuncIndex wasm.Index
	// Values are all value nodes, sorted by (Offset, Kind, Index).
	Values []*Value
	// Instructions are the reachable instruction records, sorted by Offset.
	Instructions []*Instruction
	// Edges are all control edges, sorted by (FromOffset, Kind, Selector).
	Edges []*Edge
}

// ErrorKind classifies an analysis failure.
type ErrorKind int

const (
	// ErrImportFunction means the function index refers to an imported
	// function, which has no local body to analyze.
	ErrImportFunction ErrorKind = iota
	// ErrFunctionIndex means the function index is out of range.
	ErrFunctionIndex
	// ErrUnsupportedOpcode means an instruction outside the supported set
	// was found, in live or dead code alike.
	ErrUnsupportedOpcode
	// ErrStackType means a stack underflow or a value type mismatch.
	ErrStackType
	// ErrLabelDepth means a branch label depth is out of range.
	ErrLabelDepth
	// ErrIndex means a local, type or function index is out of range.
	ErrIndex
	// ErrImmediate means a truncated or malformed immediate.
	ErrImmediate
	// ErrStructure means a block structure mismatch: dangling else/end,
	// unterminated blocks or trailing bytes.
	ErrStructure
)

// Error is a positioned analysis failure. No partial graph is returned
// alongside it.
type Error struct {
	FuncIndex wasm.Index
	Offset    uint32 // Body offset of the failure; zero when it precedes the body.
	Kind      ErrorKind
	Message   string
}

// Error implements the error interface.
func (e *Error) Error() string {
	return fmt.Sprintf("analysis: func %d, body offset %d: %s", e.FuncIndex, e.Offset, e.Message)
}

// frameKind classifies a control frame.
type frameKind int

const (
	frameFunc frameKind = iota
	frameBlock
	frameLoop
	frameIf
)

// frame is an open control frame during the single pass over the body.
type frame struct {
	kind        frameKind
	offset      uint32 // Opcode offset that opened the frame; SyntheticOffset for the function.
	params      []wasm.ValueType
	results     []wasm.ValueType
	base        int // Value stack height before the params were pushed (the frame floor).
	entryLive   bool
	paramNodes  []*Value // Non-nil when the frame was entered reachable.
	ifArgs      []*Value // Param values at the if, reused by else/false edges.
	hasElse     bool
	thenLive    bool     // The then arm was reachable at else.
	branchIn    bool     // A reachable branch targets this frame's end.
	resultNodes []*Value // Lazily created on first reachable use.
}

// analyzer holds the single-pass state.
type analyzer struct {
	m         *wasm.Module
	funcIndex wasm.Index
	body      []byte
	locals    []wasm.ValueType
	pos       int
	live      bool
	stack     []*Value // nil entries are unknown values of dead code.
	frames    []frame
	values    []*Value
	instrs    []*Instruction
	edges     []*Edge

	numFuncImports wasm.Index
}

// AnalyzeStackTransfers statically analyzes the local function at funcIndex
// in the full function index namespace: imported functions occupy the prefix
// positions, so passing the index of a function import returns an
// *Error of kind ErrImportFunction.
//
// The module, its types and the function Body are never modified, and no
// partial graph is returned on error.
func AnalyzeStackTransfers(m *wasm.Module, funcIndex wasm.Index) (*Graph, error) {
	a := &analyzer{m: m, funcIndex: funcIndex, live: true}
	for _, imp := range m.ImportSection {
		if imp != nil && imp.Type == wasm.ExternTypeFunc {
			a.numFuncImports++
		}
	}
	if funcIndex < a.numFuncImports {
		return nil, a.err(0, ErrImportFunction, "function index %d is an imported function", funcIndex)
	}
	def := funcIndex - a.numFuncImports
	if int(def) >= len(m.FunctionSection) || int(def) >= len(m.CodeSection) {
		return nil, a.err(0, ErrFunctionIndex, "function index %d has no local definition", funcIndex)
	}
	typeIdx := m.FunctionSection[def]
	if int(typeIdx) >= len(m.TypeSection) {
		return nil, a.err(0, ErrIndex, "function type index %d out of range", typeIdx)
	}
	code := m.CodeSection[def]
	if code == nil {
		return nil, a.err(0, ErrStructure, "function index %d has no code", funcIndex)
	}
	ft := m.TypeSection[typeIdx]
	a.body = code.Body
	// Copy: never alias or mutate the module's slices.
	a.locals = make([]wasm.ValueType, 0, len(ft.Params)+len(code.LocalTypes))
	a.locals = append(a.locals, ft.Params...)
	a.locals = append(a.locals, code.LocalTypes...)

	// The function itself is the outermost frame; its exit is synthetic.
	// Like in the WebAssembly validation algorithm, the body starts with an
	// empty operand stack: parameters are locals, reached via local.get.
	fn := frame{kind: frameFunc, offset: SyntheticOffset, results: ft.Results, entryLive: true}
	fn.base = 0
	a.frames = append(a.frames, fn)

	if err := a.run(); err != nil {
		return nil, err
	}

	g := &Graph{FuncIndex: funcIndex, Values: a.values, Instructions: a.instrs, Edges: a.edges}
	sortValues(g.Values)
	sort.SliceStable(g.Instructions, func(i, j int) bool { return g.Instructions[i].Offset < g.Instructions[j].Offset })
	sort.SliceStable(g.Edges, func(i, j int) bool {
		x, y := g.Edges[i], g.Edges[j]
		if x.FromOffset != y.FromOffset {
			return x.FromOffset < y.FromOffset
		}
		if x.Kind != y.Kind {
			return x.Kind < y.Kind
		}
		return x.Selector < y.Selector
	})
	return g, nil
}

// run consumes the whole body, one instruction at a time.
func (a *analyzer) run() error {
	for a.pos < len(a.body) {
		off := uint32(a.pos)
		op := a.body[a.pos]
		a.pos++
		var err error
		switch op {
		case wasm.OpcodeBlock:
			err = a.enterBlock(off, frameBlock)
		case wasm.OpcodeLoop:
			err = a.enterBlock(off, frameLoop)
		case wasm.OpcodeIf:
			err = a.enterIf(off)
		case wasm.OpcodeElse:
			err = a.else_(off)
		case wasm.OpcodeEnd:
			if err = a.end(off); err == nil && len(a.frames) == 0 {
				if a.pos != len(a.body) {
					return a.err(off, ErrStructure, "trailing bytes after function end")
				}
				return nil
			}
		case wasm.OpcodeBr:
			err = a.br(off)
		case wasm.OpcodeBrIf:
			err = a.brIf(off)
		case wasm.OpcodeBrTable:
			err = a.brTable(off)
		case wasm.OpcodeReturn:
			err = a.ret(off)
		case wasm.OpcodeUnreachable:
			a.kill(off, op)
		case wasm.OpcodeNop:
			a.nop(off, op)
		case wasm.OpcodeI32Const:
			err = a.const_(off, wasm.ValueTypeI32)
		case wasm.OpcodeI64Const:
			err = a.const_(off, wasm.ValueTypeI64)
		case wasm.OpcodeLocalGet:
			err = a.localGet(off)
		case wasm.OpcodeLocalSet:
			err = a.localSet(off)
		case wasm.OpcodeLocalTee:
			err = a.localTee(off)
		case wasm.OpcodeDrop:
			err = a.drop(off)
		case wasm.OpcodeSelect:
			err = a.select_(off)
		case wasm.OpcodeI32Add, wasm.OpcodeI32Sub, wasm.OpcodeI32Mul:
			err = a.binop(off, op, wasm.ValueTypeI32)
		case wasm.OpcodeI64Add, wasm.OpcodeI64Sub, wasm.OpcodeI64Mul:
			err = a.binop(off, op, wasm.ValueTypeI64)
		case wasm.OpcodeI32Eqz:
			err = a.eqz(off, wasm.ValueTypeI32)
		case wasm.OpcodeI64Eqz:
			err = a.eqz(off, wasm.ValueTypeI64)
		case wasm.OpcodeCall:
			err = a.call(off)
		default:
			return a.err(off, ErrUnsupportedOpcode, "unsupported opcode %#x (%s)", op, wasm.InstructionName(op))
		}
		if err != nil {
			return err
		}
	}
	return a.err(uint32(len(a.body)), ErrStructure, "body ends with %d unterminated block(s)", len(a.frames))
}

// err builds a positioned error.
func (a *analyzer) err(off uint32, kind ErrorKind, format string, args ...interface{}) *Error {
	return &Error{FuncIndex: a.funcIndex, Offset: off, Kind: kind, Message: fmt.Sprintf(format, args...)}
}

// newValue creates and registers a value node.
func (a *analyzer) newValue(kind ValueKind, off uint32, idx uint32, t wasm.ValueType) *Value {
	v := &Value{Kind: kind, Offset: off, Index: idx, Type: t}
	a.values = append(a.values, v)
	return v
}

// addInstr records a reachable instruction's inputs and outputs.
func (a *analyzer) addInstr(off uint32, op wasm.Opcode, in, out []*Value) *Instruction {
	instr := &Instruction{Offset: off, Opcode: op, Inputs: in, Outputs: out}
	a.instrs = append(a.instrs, instr)
	return instr
}

// addEdge records a control transfer.
func (a *analyzer) addEdge(from uint32, kind EdgeKind, sel uint32, tk EdgeTargetKind, tOff uint32, args, targets []*Value) {
	a.edges = append(a.edges, &Edge{FromOffset: from, Kind: kind, Selector: sel, TargetKind: tk, TargetOffset: tOff, Args: args, Targets: targets})
}

// cur returns the innermost frame index.
func (a *analyzer) cur() int { return len(a.frames) - 1 }

// popAny pops the top value. In dead code it returns nil (unknown) and never
// fails; in live code popping the frame's own floor is an underflow.
func (a *analyzer) popAny(off uint32) (*Value, error) {
	base := a.frames[a.cur()].base
	if len(a.stack) > base {
		v := a.stack[len(a.stack)-1]
		a.stack = a.stack[:len(a.stack)-1]
		return v, nil
	}
	if a.live {
		return nil, a.err(off, ErrStackType, "stack underflow")
	}
	return nil, nil
}

// popTyped pops the top value and checks its type in live code.
func (a *analyzer) popTyped(off uint32, want wasm.ValueType) (*Value, error) {
	v, err := a.popAny(off)
	if err != nil || v == nil {
		return v, err
	}
	if v.Type != want {
		return nil, a.err(off, ErrStackType, "type mismatch: have %s, want %s", wasm.ValueTypeName(v.Type), wasm.ValueTypeName(want))
	}
	return v, nil
}

// peekTyped reads the n values below the top without popping, checking types
// in live code. Used by br_if and br_table, whose fallthrough keeps the
// branch arguments on the stack.
func (a *analyzer) peekTyped(off uint32, types []wasm.ValueType) ([]*Value, error) {
	base := a.frames[a.cur()].base
	if len(a.stack)-base < len(types) {
		return nil, a.err(off, ErrStackType, "stack underflow: need %d value(s), have %d", len(types), len(a.stack)-base)
	}
	args := make([]*Value, len(types))
	for i, want := range types {
		v := a.stack[len(a.stack)-len(types)+i]
		if v.Type != want {
			return nil, a.err(off, ErrStackType, "type mismatch: have %s, want %s", wasm.ValueTypeName(v.Type), wasm.ValueTypeName(want))
		}
		args[i] = v
	}
	return args, nil
}

// push appends a value (nil in dead code).
func (a *analyzer) push(v *Value) { a.stack = append(a.stack, v) }

// kill handles unreachable: control dies, the stack resets to the frame base.
func (a *analyzer) kill(off uint32, op wasm.Opcode) {
	if a.live {
		a.addInstr(off, op, nil, nil)
	}
	a.truncateToBase()
	a.live = false
}

// nop handles nop.
func (a *analyzer) nop(off uint32, op wasm.Opcode) {
	if a.live {
		a.addInstr(off, op, nil, nil)
	}
}

// truncateToBase resets the stack to the innermost frame's floor.
func (a *analyzer) truncateToBase() {
	a.stack = a.stack[:a.frames[a.cur()].base]
}

// readUint32 reads a LEB128 uint32 immediate.
func (a *analyzer) readUint32(off uint32, what string) (uint32, error) {
	v, n, err := leb128.DecodeUint32(bytes.NewReader(a.body[a.pos:]))
	if err != nil {
		return 0, a.err(off, ErrImmediate, "truncated or malformed %s immediate: %v", what, err)
	}
	a.pos += int(n)
	return v, nil
}

// readInt32 reads a signed LEB128 int32 immediate.
func (a *analyzer) readInt32(off uint32) (int32, error) {
	v, n, err := leb128.DecodeInt32(bytes.NewReader(a.body[a.pos:]))
	if err != nil {
		return 0, a.err(off, ErrImmediate, "truncated or malformed i32.const immediate: %v", err)
	}
	a.pos += int(n)
	return v, nil
}

// readInt64 reads a signed LEB128 int64 immediate.
func (a *analyzer) readInt64(off uint32) (int64, error) {
	v, n, err := leb128.DecodeInt64(bytes.NewReader(a.body[a.pos:]))
	if err != nil {
		return 0, a.err(off, ErrImmediate, "truncated or malformed i64.const immediate: %v", err)
	}
	a.pos += int(n)
	return v, nil
}

// blockType decodes a block type immediate: the empty type (0x40), a single
// result value type, or a signed LEB33 type index for multi-value types.
func (a *analyzer) blockType(off uint32) (params, results []wasm.ValueType, err error) {
	v, n, e := leb128.DecodeInt33AsInt64(bytes.NewReader(a.body[a.pos:]))
	if e != nil {
		return nil, nil, a.err(off, ErrImmediate, "truncated or malformed block type: %v", e)
	}
	a.pos += int(n)
	switch {
	case v == -64: // 0x40: empty block type.
		return nil, nil, nil
	case v < 0:
		vt := wasm.ValueType(byte(v + 128))
		switch vt {
		case wasm.ValueTypeI32, wasm.ValueTypeI64, wasm.ValueTypeF32, wasm.ValueTypeF64,
			wasm.ValueTypeExternref, wasm.ValueTypeFuncref, wasm.ValueTypeV128:
			return nil, []wasm.ValueType{vt}, nil
		}
		return nil, nil, a.err(off, ErrStructure, "invalid block type %#x", byte(v+128))
	default:
		if v >= int64(len(a.m.TypeSection)) {
			return nil, nil, a.err(off, ErrIndex, "block type index %d out of range", v)
		}
		ft := a.m.TypeSection[v]
		return ft.Params, ft.Results, nil
	}
}

// frameResultNodes lazily creates the merge parameter nodes of a frame, so
// their identity is stable no matter which incoming edge is seen first.
func (a *analyzer) frameResultNodes(fi int) []*Value {
	f := &a.frames[fi]
	if f.resultNodes == nil {
		kind := ValueBlockResult
		if f.kind == frameFunc {
			kind = ValueFuncResult
		}
		nodes := make([]*Value, len(f.results))
		for i, t := range f.results {
			nodes[i] = a.newValue(kind, f.offset, uint32(i), t)
		}
		f.resultNodes = nodes
	}
	return f.resultNodes
}

// branchTarget resolves a frame as a branch target: a loop receives its
// header params, anything else its end results.
func (a *analyzer) branchTarget(fi int) (types []wasm.ValueType, tk EdgeTargetKind, targets []*Value) {
	f := &a.frames[fi]
	if f.kind == frameLoop {
		return f.params, TargetBlockParam, f.paramNodes
	}
	if f.kind == frameFunc {
		return f.results, TargetFuncResult, a.frameResultNodes(fi)
	}
	return f.results, TargetBlockResult, a.frameResultNodes(fi)
}

// labelFrame maps a label depth to a frame index, checking the depth.
func (a *analyzer) labelFrame(off uint32, depth uint32) (int, error) {
	if uint64(depth) >= uint64(len(a.frames)) {
		return 0, a.err(off, ErrLabelDepth, "label depth %d out of range (%d frame(s))", depth, len(a.frames))
	}
	return len(a.frames) - 1 - int(depth), nil
}

// enterBlock handles block and loop.
func (a *analyzer) enterBlock(off uint32, kind frameKind) error {
	params, results, err := a.blockType(off)
	if err != nil {
		return err
	}
	f := frame{kind: kind, offset: off, params: params, results: results, entryLive: a.live}
	if a.live {
		args, err := a.popParams(off, params)
		if err != nil {
			return err
		}
		f.base = len(a.stack) // the floor excludes the params
		f.paramNodes = make([]*Value, len(params))
		for i, t := range params {
			f.paramNodes[i] = a.newValue(ValueBlockParam, off, uint32(i), t)
		}
		for _, n := range f.paramNodes {
			a.push(n)
		}
		a.addEdge(off, EdgeEnter, 0, TargetBlockParam, off, args, f.paramNodes)
		a.addInstr(off, a.body[off], args, f.paramNodes)
	} else {
		for range params {
			if _, err := a.popAny(off); err != nil {
				return err
			}
		}
		f.base = len(a.stack)
		for range params {
			a.push(nil)
		}
	}
	a.frames = append(a.frames, f)
	return nil
}

// enterIf handles if: the condition is consumed before the block params.
func (a *analyzer) enterIf(off uint32) error {
	params, results, err := a.blockType(off)
	if err != nil {
		return err
	}
	f := frame{kind: frameIf, offset: off, params: params, results: results, entryLive: a.live}
	if a.live {
		cond, err := a.popTyped(off, wasm.ValueTypeI32)
		if err != nil {
			return err
		}
		args, err := a.popParams(off, params)
		if err != nil {
			return err
		}
		f.base = len(a.stack) // the floor excludes the params
		f.paramNodes = make([]*Value, len(params))
		for i, t := range params {
			f.paramNodes[i] = a.newValue(ValueBlockParam, off, uint32(i), t)
		}
		for _, n := range f.paramNodes {
			a.push(n)
		}
		f.ifArgs = args
		a.addEdge(off, EdgeEnter, 0, TargetBlockParam, off, args, f.paramNodes)
		in := make([]*Value, 0, len(args)+1)
		in = append(in, args...)
		in = append(in, cond)
		a.addInstr(off, a.body[off], in, f.paramNodes)
	} else {
		if _, err := a.popAny(off); err != nil { // condition
			return err
		}
		for range params {
			if _, err := a.popAny(off); err != nil {
				return err
			}
		}
		f.base = len(a.stack)
		for range params {
			a.push(nil)
		}
	}
	a.frames = append(a.frames, f)
	return nil
}

// popParams pops a declared parameter sequence, returning args bottom-to-top.
func (a *analyzer) popParams(off uint32, params []wasm.ValueType) ([]*Value, error) {
	args := make([]*Value, len(params))
	for i := len(params) - 1; i >= 0; i-- {
		v, err := a.popTyped(off, params[i])
		if err != nil {
			return nil, err
		}
		args[i] = v
	}
	return args, nil
}

// popResults pops a frame's results at a live end or else, requiring the
// stack to hold exactly the results above the frame floor.
func (a *analyzer) popResults(off uint32, f *frame) ([]*Value, error) {
	if len(a.stack) != f.base+len(f.results) {
		return nil, a.err(off, ErrStackType, "stack holds %d value(s) above the block, want exactly %d result(s)", len(a.stack)-f.base, len(f.results))
	}
	return a.popParams(off, f.results)
}

// else_ handles else: the then arm merges into the block results and the
// else arm restarts from the same input stack as the then arm.
func (a *analyzer) else_(off uint32) error {
	fi := a.cur()
	f := &a.frames[fi]
	if f.kind != frameIf || f.hasElse {
		return a.err(off, ErrStructure, "else without a matching if")
	}
	f.hasElse = true
	if a.live {
		args, err := a.popResults(off, f)
		if err != nil {
			return err
		}
		a.addEdge(off, EdgeEnd, 0, TargetBlockResult, f.offset, args, a.frameResultNodes(fi))
		a.addInstr(off, a.body[off], args, nil)
	}
	f.thenLive = a.live
	// The else arm restarts from the block params.
	a.stack = a.stack[:f.base]
	if f.paramNodes != nil {
		for _, n := range f.paramNodes {
			a.push(n)
		}
	} else {
		for range f.params {
			a.push(nil)
		}
	}
	if f.entryLive {
		a.addEdge(off, EdgeEnterElse, 0, TargetBlockParam, f.offset, f.ifArgs, f.paramNodes)
	}
	a.live = f.entryLive
	return nil
}

// end handles end, including the implicit function end.
func (a *analyzer) end(off uint32) error {
	fi := a.cur()
	f := &a.frames[fi]
	if f.kind == frameIf && !f.hasElse && !equalTypes(f.params, f.results) {
		return a.err(off, ErrStructure, "if without else requires params to match results")
	}
	if a.live {
		args, err := a.popResults(off, f)
		if err != nil {
			return err
		}
		tk := TargetBlockResult
		if f.kind == frameFunc {
			tk = TargetFuncResult
		}
		targets := a.frameResultNodes(fi)
		a.addEdge(off, EdgeEnd, 0, tk, f.offset, args, targets)
		a.addInstr(off, a.body[off], args, targets)
	}
	if f.kind == frameIf && !f.hasElse && f.entryLive {
		// An if without else has a valid false edge: the inputs flow
		// directly to the results.
		a.addEdge(f.offset, EdgeFalse, 0, TargetBlockResult, f.offset, f.ifArgs, a.frameResultNodes(fi))
	}
	// An end is reachable again when a branch already targeted it, even if
	// the previous instruction was not reachable.
	endLive := a.live || (f.kind != frameLoop && f.branchIn)
	if f.kind == frameIf {
		if f.hasElse {
			endLive = endLive || f.thenLive
		} else {
			endLive = endLive || f.entryLive
		}
	}
	// Pop the frame: the params were consumed by the body, the results are
	// produced onto the parent stack.
	a.stack = a.stack[:f.base]
	if endLive {
		for _, n := range a.frameResultNodes(fi) {
			a.push(n)
		}
	} else {
		for range f.results {
			a.push(nil)
		}
	}
	a.frames = a.frames[:fi]
	a.live = endLive
	return nil
}

// br handles an unconditional branch. The stack is polymorphic afterwards.
func (a *analyzer) br(off uint32) error {
	depth, err := a.readUint32(off, "br label")
	if err != nil {
		return err
	}
	fi, err := a.labelFrame(off, depth)
	if err != nil {
		return err
	}
	if a.live {
		types, tk, targets := a.branchTarget(fi)
		args, err := a.popParams(off, types)
		if err != nil {
			return err
		}
		a.addEdge(off, EdgeBranch, 0, tk, a.frames[fi].offset, args, targets)
		a.addInstr(off, a.body[off], args, nil)
		a.noteBranchIn(fi)
	}
	a.truncateToBase()
	a.live = false
	return nil
}

// brIf handles a conditional branch: the condition is consumed first, the
// taken edge carries the target's top-of-stack arguments, and the
// fallthrough keeps them.
func (a *analyzer) brIf(off uint32) error {
	depth, err := a.readUint32(off, "br_if label")
	if err != nil {
		return err
	}
	fi, err := a.labelFrame(off, depth)
	if err != nil {
		return err
	}
	if a.live {
		cond, err := a.popTyped(off, wasm.ValueTypeI32)
		if err != nil {
			return err
		}
		types, tk, targets := a.branchTarget(fi)
		args, err := a.peekTyped(off, types)
		if err != nil {
			return err
		}
		a.addEdge(off, EdgeBranchIf, 0, tk, a.frames[fi].offset, args, targets)
		a.addInstr(off, a.body[off], []*Value{cond}, nil)
		a.noteBranchIn(fi)
	} else {
		if _, err := a.popAny(off); err != nil { // condition
			return err
		}
	}
	return nil
}

// brTable handles a branch table. Every selector and the default get their
// own edge even when targets repeat; all targets must require the same type
// sequence.
func (a *analyzer) brTable(off uint32) error {
	count, err := a.readUint32(off, "br_table count")
	if err != nil {
		return err
	}
	// Each target and the default need at least one byte: reject absurd
	// counts before allocating, so a malformed body cannot cause a huge
	// allocation.
	if uint64(count)+1 > uint64(len(a.body)-a.pos) {
		return a.err(off, ErrImmediate, "br_table count %d exceeds the remaining body", count)
	}
	depths := make([]uint32, 0, count+1)
	for i := uint32(0); i < count; i++ {
		d, err := a.readUint32(off, "br_table label")
		if err != nil {
			return err
		}
		depths = append(depths, d)
	}
	def, err := a.readUint32(off, "br_table default label")
	if err != nil {
		return err
	}
	depths = append(depths, def)
	fis := make([]int, len(depths))
	for i, d := range depths {
		fi, err := a.labelFrame(off, d)
		if err != nil {
			return err
		}
		fis[i] = fi
	}
	// All targets must require the same type sequence, in dead code too.
	// The check reads frame types directly: it must not create value nodes.
	var want []wasm.ValueType
	for i, fi := range fis {
		f := &a.frames[fi]
		types := f.results
		if f.kind == frameLoop {
			types = f.params
		}
		if i == 0 {
			want = types
			continue
		}
		if !equalTypes(types, want) {
			return a.err(off, ErrStackType, "br_table targets require different type sequences")
		}
	}
	if a.live {
		idx, err := a.popTyped(off, wasm.ValueTypeI32)
		if err != nil {
			return err
		}
		args, err := a.peekTyped(off, want)
		if err != nil {
			return err
		}
		for i, fi := range fis {
			_, tk, targets := a.branchTarget(fi)
			a.addEdge(off, EdgeBranchTable, uint32(i), tk, a.frames[fi].offset, args, targets)
			a.noteBranchIn(fi)
		}
		a.addInstr(off, a.body[off], []*Value{idx}, nil)
		a.truncateToBase()
		a.live = false
	} else {
		if _, err := a.popAny(off); err != nil { // index
			return err
		}
	}
	return nil
}

// ret handles return: a branch to the synthetic function exit.
func (a *analyzer) ret(off uint32) error {
	if a.live {
		types, tk, targets := a.branchTarget(0)
		args, err := a.popParams(off, types)
		if err != nil {
			return err
		}
		a.addEdge(off, EdgeReturn, 0, tk, a.frames[0].offset, args, targets)
		a.addInstr(off, a.body[off], args, nil)
		a.frames[0].branchIn = true
	}
	a.truncateToBase()
	a.live = false
	return nil
}

// noteBranchIn marks a reachable branch into a frame's end.
func (a *analyzer) noteBranchIn(fi int) {
	if a.frames[fi].kind != frameLoop {
		a.frames[fi].branchIn = true
	}
}

// const_ handles i32.const and i64.const.
func (a *analyzer) const_(off uint32, vt wasm.ValueType) error {
	var err error
	if vt == wasm.ValueTypeI32 {
		_, err = a.readInt32(off)
	} else {
		_, err = a.readInt64(off)
	}
	if err != nil {
		return err
	}
	if a.live {
		v := a.newValue(ValueInstr, off, 0, vt)
		a.push(v)
		a.addInstr(off, a.body[off], nil, []*Value{v})
	} else {
		a.push(nil)
	}
	return nil
}

// localGet handles local.get: an external value source at this position.
func (a *analyzer) localGet(off uint32) error {
	idx, err := a.readUint32(off, "local index")
	if err != nil {
		return err
	}
	if int(idx) >= len(a.locals) {
		return a.err(off, ErrIndex, "local index %d out of range (%d local(s))", idx, len(a.locals))
	}
	if a.live {
		v := a.newValue(ValueLocalGet, off, 0, a.locals[idx])
		a.push(v)
		a.addInstr(off, a.body[off], nil, []*Value{v})
	} else {
		a.push(nil)
	}
	return nil
}

// localSet handles local.set.
func (a *analyzer) localSet(off uint32) error {
	idx, err := a.readUint32(off, "local index")
	if err != nil {
		return err
	}
	if int(idx) >= len(a.locals) {
		return a.err(off, ErrIndex, "local index %d out of range (%d local(s))", idx, len(a.locals))
	}
	if a.live {
		v, err := a.popTyped(off, a.locals[idx])
		if err != nil {
			return err
		}
		a.addInstr(off, a.body[off], []*Value{v}, nil)
	} else {
		if _, err := a.popAny(off); err != nil {
			return err
		}
	}
	return nil
}

// localTee handles local.tee: the value stays on the stack.
func (a *analyzer) localTee(off uint32) error {
	idx, err := a.readUint32(off, "local index")
	if err != nil {
		return err
	}
	if int(idx) >= len(a.locals) {
		return a.err(off, ErrIndex, "local index %d out of range (%d local(s))", idx, len(a.locals))
	}
	if a.live {
		base := a.frames[a.cur()].base
		if len(a.stack) <= base {
			return a.err(off, ErrStackType, "stack underflow")
		}
		v := a.stack[len(a.stack)-1]
		if want := a.locals[idx]; v.Type != want {
			return a.err(off, ErrStackType, "type mismatch: have %s, want %s", wasm.ValueTypeName(v.Type), wasm.ValueTypeName(want))
		}
		a.addInstr(off, a.body[off], []*Value{v}, []*Value{v})
	}
	return nil
}

// drop handles drop.
func (a *analyzer) drop(off uint32) error {
	if a.live {
		v, err := a.popAny(off)
		if err != nil {
			return err
		}
		a.addInstr(off, a.body[off], []*Value{v}, nil)
	} else {
		if _, err := a.popAny(off); err != nil {
			return err
		}
	}
	return nil
}

// select_ handles select: the condition is consumed from the top, then the
// two operands, which must share a type.
func (a *analyzer) select_(off uint32) error {
	if a.live {
		cond, err := a.popTyped(off, wasm.ValueTypeI32)
		if err != nil {
			return err
		}
		v2, err := a.popAny(off)
		if err != nil {
			return err
		}
		v1, err := a.popAny(off)
		if err != nil {
			return err
		}
		if v1.Type != v2.Type {
			return a.err(off, ErrStackType, "select operands have different types: %s and %s", wasm.ValueTypeName(v1.Type), wasm.ValueTypeName(v2.Type))
		}
		v := a.newValue(ValueInstr, off, 0, v1.Type)
		a.push(v)
		a.addInstr(off, a.body[off], []*Value{v1, v2, cond}, []*Value{v})
	} else {
		for i := 0; i < 3; i++ {
			if _, err := a.popAny(off); err != nil {
				return err
			}
		}
		a.push(nil)
	}
	return nil
}

// binop handles i32/i64 add, sub and mul.
func (a *analyzer) binop(off uint32, op wasm.Opcode, vt wasm.ValueType) error {
	if a.live {
		b, err := a.popTyped(off, vt)
		if err != nil {
			return err
		}
		x, err := a.popTyped(off, vt)
		if err != nil {
			return err
		}
		v := a.newValue(ValueInstr, off, 0, vt)
		a.push(v)
		a.addInstr(off, op, []*Value{x, b}, []*Value{v})
	} else {
		for i := 0; i < 2; i++ {
			if _, err := a.popAny(off); err != nil {
				return err
			}
		}
		a.push(nil)
	}
	return nil
}

// eqz handles i32.eqz and i64.eqz.
func (a *analyzer) eqz(off uint32, vt wasm.ValueType) error {
	if a.live {
		x, err := a.popTyped(off, vt)
		if err != nil {
			return err
		}
		v := a.newValue(ValueInstr, off, 0, wasm.ValueTypeI32)
		a.push(v)
		a.addInstr(off, a.body[off], []*Value{x}, []*Value{v})
	} else {
		if _, err := a.popAny(off); err != nil {
			return err
		}
		a.push(nil)
	}
	return nil
}

// call handles a direct call: it consumes arguments and produces results per
// the real signature, without following the callee. Imported callees are
// described by their declared type; nothing is executed.
func (a *analyzer) call(off uint32) error {
	idx, err := a.readUint32(off, "call function index")
	if err != nil {
		return err
	}
	sig, err := a.funcType(off, idx)
	if err != nil {
		return err
	}
	if a.live {
		args, err := a.popParams(off, sig.Params)
		if err != nil {
			return err
		}
		out := make([]*Value, len(sig.Results))
		for i, t := range sig.Results {
			out[i] = a.newValue(ValueInstr, off, uint32(i), t)
		}
		for _, v := range out {
			a.push(v)
		}
		instr := a.addInstr(off, a.body[off], args, out)
		instr.CallTarget = idx
	} else {
		for range sig.Params {
			if _, err := a.popAny(off); err != nil {
				return err
			}
		}
		for range sig.Results {
			a.push(nil)
		}
	}
	return nil
}

// funcType resolves the signature of a function index in the full namespace.
func (a *analyzer) funcType(off uint32, idx wasm.Index) (*wasm.FunctionType, error) {
	if idx < a.numFuncImports {
		var n wasm.Index
		for _, imp := range a.m.ImportSection {
			if imp == nil || imp.Type != wasm.ExternTypeFunc {
				continue
			}
			if n == idx {
				if int(imp.DescFunc) >= len(a.m.TypeSection) {
					return nil, a.err(off, ErrIndex, "imported function %d has type index %d out of range", idx, imp.DescFunc)
				}
				return a.m.TypeSection[imp.DescFunc], nil
			}
			n++
		}
		return nil, a.err(off, ErrIndex, "function index %d out of range", idx)
	}
	def := idx - a.numFuncImports
	if int(def) >= len(a.m.FunctionSection) {
		return nil, a.err(off, ErrIndex, "function index %d out of range", idx)
	}
	typeIdx := a.m.FunctionSection[def]
	if int(typeIdx) >= len(a.m.TypeSection) {
		return nil, a.err(off, ErrIndex, "function %d has type index %d out of range", idx, typeIdx)
	}
	return a.m.TypeSection[typeIdx], nil
}

// equalTypes compares two value type sequences.
func equalTypes(a, b []wasm.ValueType) bool { return bytes.Equal(a, b) }

// sortValues orders value nodes by (Offset, Kind, Index).
func sortValues(vs []*Value) {
	sort.SliceStable(vs, func(i, j int) bool {
		x, y := vs[i], vs[j]
		if x.Offset != y.Offset {
			return x.Offset < y.Offset
		}
		if x.Kind != y.Kind {
			return x.Kind < y.Kind
		}
		return x.Index < y.Index
	})
}
