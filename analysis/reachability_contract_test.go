package analysis

import (
	"strings"
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

func TestReachabilityContract(t *testing.T) {
	t.Run("initialization", testReachabilityInitialization)
	t.Run("fixedpoint", testReachabilityFixedPoint)
	t.Run("copy", testReachabilityCopy)
	t.Run("signature", testReachabilitySignature)
	t.Run("external", testReachabilityExternal)
	t.Run("malformed", testReachabilityMalformed)
}

func indexPtr(i wasm.Index) *wasm.Index { return &i }

func activeSegment(table wasm.Index, offset int32, init []*wasm.Index) *wasm.ElementSegment {
	return &wasm.ElementSegment{
		Type:       wasm.RefTypeFuncref,
		Mode:       wasm.ElementModeActive,
		TableIndex: table,
		OffsetExpr: &wasm.ConstantExpression{
			Opcode: wasm.OpcodeI32Const,
			Data:   leb128.EncodeInt32(offset),
		},
		Init: init,
	}
}

func tableInfo(g *Graph, idx wasm.Index) *TableInfo {
	for i := range g.Tables {
		if g.Tables[i].Index == idx {
			return &g.Tables[i]
		}
	}
	return nil
}

func callTargets(g *Graph, caller wasm.Index, offset int) []wasm.Index {
	var out []wasm.Index
	for _, c := range g.Calls {
		if c.Caller == caller && c.Offset == offset {
			out = append(out, c.Target)
		}
	}
	return out
}

func callSites(g *Graph, caller wasm.Index, offset int) []CallSite {
	var out []CallSite
	for _, c := range g.Calls {
		if c.Caller == caller && c.Offset == offset {
			out = append(out, c)
		}
	}
	return out
}

func reachableIndices(g *Graph) []wasm.Index {
	out := make([]wasm.Index, 0, len(g.Functions))
	for _, f := range g.Functions {
		out = append(out, f.Index)
	}
	return out
}

func requireFunctionError(t *testing.T, err error, fn wasm.Index, offset int, contains string) {
	t.Helper()
	require.Error(t, err)
	ae, ok := err.(*Error)
	require.True(t, ok, "expected *analysis.Error, got %T: %v", err, err)
	require.True(t, ae.IsFunction, "expected a function body error")
	require.Equal(t, fn, ae.Function)
	require.Equal(t, offset, ae.Offset)
	require.Contains(t, ae.Message, contains)
}

func requireSegmentError(t *testing.T, err error, segment int, contains string) {
	t.Helper()
	require.Error(t, err)
	ae, ok := err.(*Error)
	require.True(t, ok, "expected *analysis.Error, got %T: %v", err, err)
	require.False(t, ae.IsFunction)
	require.Equal(t, segment, ae.Segment)
	require.Contains(t, ae.Message, contains)
}

// testReachabilityInitialization covers active element application order,
// overwriting, null slots, passive/declarative exclusion, global.get offsets,
// block/br_table immediate consumption, byte offsets, sparse large tables,
// and result ownership.
func testReachabilityInitialization(t *testing.T) {
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{}, // type 0: () -> ()
			{Params: []wasm.ValueType{wasm.ValueTypeI32}, Results: []wasm.ValueType{wasm.ValueTypeI32}}, // type 1
		},
		ImportSection: []*wasm.Import{{
			Module: "host", Name: "f", Type: wasm.ExternTypeFunc, DescFunc: 1,
		}},
		FunctionSection: []wasm.Index{0, 0, 0, 0, 1}, // local index 1..5
		TableSection: []*wasm.Table{
			{Type: wasm.RefTypeFuncref, Min: 4},
			{Type: wasm.RefTypeFuncref, Min: 1},
		},
		GlobalSection: []*wasm.Global{{
			Type: &wasm.GlobalType{ValType: wasm.ValueTypeI32},
			Init: &wasm.ConstantExpression{Opcode: wasm.OpcodeI32Const, Data: leb128.EncodeInt32(0)},
		}},
		ElementSection: []*wasm.ElementSegment{
			// slot 1=f1, slot 2=f2
			activeSegment(0, 1, []*wasm.Index{indexPtr(1), indexPtr(2)}),
			// slot 2=null (overwrites f2), slot 3=f3
			activeSegment(0, 2, []*wasm.Index{nil, indexPtr(3)}),
			// passive segments never initialize a table
			{Type: wasm.RefTypeFuncref, Mode: wasm.ElementModePassive, Init: []*wasm.Index{indexPtr(4)}},
			// declarative segments never initialize a table
			{Type: wasm.RefTypeFuncref, Mode: wasm.ElementModeDeclarative, Init: []*wasm.Index{indexPtr(5)}},
			// global.get offset makes the table unknown
			{
				Type:       wasm.RefTypeFuncref,
				Mode:       wasm.ElementModeActive,
				TableIndex: 1,
				OffsetExpr: &wasm.ConstantExpression{Opcode: wasm.OpcodeGlobalGet, Data: leb128.EncodeUint32(0)},
				Init:       []*wasm.Index{indexPtr(4)},
			},
		},
		CodeSection: []*wasm.Code{
			// block (type 0); br_table 2 [0 0 0]; end; i32.const 0; call_indirect type 0 table 0; end
			{Body: []byte{
				wasm.OpcodeBlock, 0x00,
				wasm.OpcodeBrTable, 0x02, 0x00, 0x00, 0x00,
				wasm.OpcodeEnd,
				wasm.OpcodeI32Const, 0x00,
				wasm.OpcodeCallIndirect, 0x00, 0x00,
				wasm.OpcodeEnd,
			}},
			{Body: []byte{wasm.OpcodeEnd}}, // f2, overwritten in table slot 2
			{Body: []byte{wasm.OpcodeEnd}}, // f3
			{Body: []byte{wasm.OpcodeEnd}}, // f4, only in passive segment
			{Body: []byte{wasm.OpcodeEnd}}, // f5, only in declarative segment
		},
	}

	pristine := initializationFixture(t)

	g, err := BuildReachability(m, []wasm.Index{1, 1})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{1}, g.Roots, "duplicate roots are deduplicated")

	// Reachable: root 1 and indirect target 3. Function 2 was overwritten by
	// a later null slot, while 4 and 5 only appear in non-active segments.
	require.Equal(t, []wasm.Index{1, 3}, reachableIndices(g))
	require.Equal(t, []wasm.Index{1}, g.CallPath(1))
	require.Equal(t, []wasm.Index{1, 3}, g.CallPath(3))
	require.Nil(t, g.CallPath(2))

	// The call_indirect is at body offset 10: block(2) + br_table(5) + end(1)
	// + i32.const(2) precede it.
	sites := callSites(g, 1, 10)
	require.Len(t, sites, 2)
	for _, s := range sites {
		require.Equal(t, CallIndirect, s.Kind)
		require.Equal(t, wasm.Index(0), s.TableIndex)
		require.Equal(t, wasm.Index(0), s.TypeIndex)
		require.Empty(t, s.UnknownReason)
		require.False(t, s.PossibleExternal)
	}
	require.Equal(t, []wasm.Index{1, 3}, callTargets(g, 1, 10))

	ti := tableInfo(g, 0)
	require.NotNil(t, ti)
	require.False(t, ti.Unknown)
	require.False(t, ti.PossibleExternal)
	require.Equal(t, []wasm.Index{1, 3}, ti.Candidates)
	require.Len(t, ti.Expansions, 2)
	require.Equal(t, ExpansionActiveElement, ti.Expansions[0].Kind)
	require.Equal(t, 0, ti.Expansions[0].SegmentIndex)
	require.Equal(t, ExpansionActiveElement, ti.Expansions[1].Kind)
	require.Equal(t, 1, ti.Expansions[1].SegmentIndex)

	ti1 := tableInfo(g, 1)
	require.NotNil(t, ti1)
	require.True(t, ti1.Unknown)
	require.Empty(t, ti1.Candidates)
	require.Condition(t, func() bool {
		for _, r := range ti1.UnknownReasons {
			if strings.Contains(r, "global.get") {
				return true
			}
		}
		return false
	}, "global.get unknown reason: %v", ti1.UnknownReasons)

	// The graph must not mutate the module.
	require.Equal(t, pristine, m)

	// Results own their slices: mutating one graph cannot affect another.
	g2, err := BuildReachability(m, []wasm.Index{1})
	require.NoError(t, err)
	g.Tables[0].Candidates[0] = 99
	g.Functions[0].Path[0] = 99
	require.Equal(t, []wasm.Index{1, 3}, g2.Tables[0].Candidates)
	require.Equal(t, []wasm.Index{1}, g2.Functions[0].Path)
	require.Equal(t, pristine, m)

	// Large minima use sparse storage rather than a slot array.
	big := &wasm.Module{
		TypeSection:     []*wasm.FunctionType{{}},
		FunctionSection: []wasm.Index{0},
		CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
		TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 1 << 27}},
		ElementSection:  []*wasm.ElementSegment{activeSegment(0, 1<<26, []*wasm.Index{indexPtr(0)})},
	}
	bigGraph, err := BuildReachability(big, []wasm.Index{0})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{0}, tableInfo(bigGraph, 0).Candidates)
}

// initializationFixture returns a fresh copy of the module in
// testReachabilityInitialization.
func initializationFixture(t *testing.T) *wasm.Module {
	t.Helper()
	return &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{},
			{Params: []wasm.ValueType{wasm.ValueTypeI32}, Results: []wasm.ValueType{wasm.ValueTypeI32}},
		},
		ImportSection: []*wasm.Import{{
			Module: "host", Name: "f", Type: wasm.ExternTypeFunc, DescFunc: 1,
		}},
		FunctionSection: []wasm.Index{0, 0, 0, 0, 1},
		TableSection: []*wasm.Table{
			{Type: wasm.RefTypeFuncref, Min: 4},
			{Type: wasm.RefTypeFuncref, Min: 1},
		},
		GlobalSection: []*wasm.Global{{
			Type: &wasm.GlobalType{ValType: wasm.ValueTypeI32},
			Init: &wasm.ConstantExpression{Opcode: wasm.OpcodeI32Const, Data: leb128.EncodeInt32(0)},
		}},
		ElementSection: []*wasm.ElementSegment{
			activeSegment(0, 1, []*wasm.Index{indexPtr(1), indexPtr(2)}),
			activeSegment(0, 2, []*wasm.Index{nil, indexPtr(3)}),
			{Type: wasm.RefTypeFuncref, Mode: wasm.ElementModePassive, Init: []*wasm.Index{indexPtr(4)}},
			{Type: wasm.RefTypeFuncref, Mode: wasm.ElementModeDeclarative, Init: []*wasm.Index{indexPtr(5)}},
			{
				Type:       wasm.RefTypeFuncref,
				Mode:       wasm.ElementModeActive,
				TableIndex: 1,
				OffsetExpr: &wasm.ConstantExpression{Opcode: wasm.OpcodeGlobalGet, Data: leb128.EncodeUint32(0)},
				Init:       []*wasm.Index{indexPtr(4)},
			},
		},
		CodeSection: []*wasm.Code{
			{Body: []byte{
				wasm.OpcodeBlock, 0x00,
				wasm.OpcodeBrTable, 0x02, 0x00, 0x00, 0x00,
				wasm.OpcodeEnd,
				wasm.OpcodeI32Const, 0x00,
				wasm.OpcodeCallIndirect, 0x00, 0x00,
				wasm.OpcodeEnd,
			}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
		},
	}
}

// testReachabilityFixedPoint covers an indirectly discovered writer that makes
// an already processed call site gain targets, and an unreachable writer that
// must not pollute another table.
func testReachabilityFixedPoint(t *testing.T) {
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{}, // type 0: () -> ()
			{Params: []wasm.ValueType{wasm.ValueTypeI32}, Results: []wasm.ValueType{wasm.ValueTypeI32}}, // type 1
		},
		ImportSection: []*wasm.Import{{
			Module: "host", Name: "hostfn", Type: wasm.ExternTypeFunc, DescFunc: 1,
		}},
		FunctionSection: []wasm.Index{0, 0, 0, 0, 1}, // local indices 1 (root), 2, 3 (writer), 4, 5 (unreachable writer)
		TableSection: []*wasm.Table{
			{Type: wasm.RefTypeFuncref, Min: 2},
			{Type: wasm.RefTypeFuncref, Min: 1},
		},
		ElementSection: []*wasm.ElementSegment{
			activeSegment(0, 0, []*wasm.Index{indexPtr(2)}),
		},
		CodeSection: []*wasm.Code{
			// f1 (root): operand; call_indirect type 0 table 0
			{Body: []byte{wasm.OpcodeI32Const, 0x00, wasm.OpcodeCallIndirect, 0x00, 0x00, wasm.OpcodeEnd}},
			// f2: call 3
			{Body: []byte{wasm.OpcodeCall, 0x03, wasm.OpcodeEnd}},
			// f3: i32.const 0; ref.null funcref; table.set 0
			{Body: []byte{
				wasm.OpcodeI32Const, 0x00, wasm.OpcodeRefNull, wasm.RefTypeFuncref,
				wasm.OpcodeTableSet, 0x00, wasm.OpcodeEnd,
			}},
			{Body: []byte{wasm.OpcodeEnd}}, // f4
			// f5 (unreachable): table.set 1
			{Body: []byte{
				wasm.OpcodeI32Const, 0x00, wasm.OpcodeRefNull, wasm.RefTypeFuncref,
				wasm.OpcodeTableSet, 0x01, wasm.OpcodeEnd,
			}},
		},
	}

	g, err := BuildReachability(m, []wasm.Index{1})
	require.NoError(t, err)

	// f2 is found from the table, f3 from f2's direct call, then f3 makes
	// table 0 unknown, which re-expands the old call site and adds f1..f4.
	require.Equal(t, []wasm.Index{1, 2, 3, 4}, reachableIndices(g))
	require.Equal(t, []wasm.Index{1, 2, 3}, g.CallPath(3))
	require.Equal(t, []wasm.Index{1, 4}, g.CallPath(4))

	// The direct call from f2 to f3 is at offset 0.
	direct := callSites(g, 2, 0)
	require.Len(t, direct, 1)
	require.Equal(t, CallDirect, direct[0].Kind)
	require.Equal(t, wasm.Index(3), direct[0].Target)

	// The call_indirect at f1 offset 2 initially only reached f2, then
	// re-expansion added 1, 3 and 4.
	require.Equal(t, []wasm.Index{1, 2, 3, 4}, callTargets(g, 1, 2))
	for _, s := range callSites(g, 1, 2) {
		require.Equal(t, CallIndirect, s.Kind)
		require.Equal(t, wasm.Index(0), s.TableIndex)
		require.Equal(t, wasm.Index(0), s.TypeIndex)
		require.NotEmpty(t, s.UnknownReason)
		require.True(t, s.PossibleExternal)
	}

	ti := tableInfo(g, 0)
	require.True(t, ti.Unknown)
	require.True(t, ti.PossibleExternal)
	require.NotEmpty(t, ti.UnknownReasons)

	// Trace the runtime write back to reachable f3 at body offset 4.
	var setExpansion *TableExpansion
	for i := range ti.Expansions {
		if ti.Expansions[i].Kind == ExpansionTableSet {
			setExpansion = &ti.Expansions[i]
		}
	}
	require.NotNil(t, setExpansion)
	require.True(t, setExpansion.HasWriter)
	require.Equal(t, wasm.Index(3), setExpansion.Writer)
	require.Equal(t, 4, setExpansion.Offset)
	require.Equal(t, -1, setExpansion.SegmentIndex)

	// Unreachable writer in f5 targets table 1 and must not affect it.
	ti1 := tableInfo(g, 1)
	require.NotNil(t, ti1)
	require.False(t, ti1.Unknown)
	require.False(t, ti1.PossibleExternal)
	require.Empty(t, ti1.Candidates)
	require.Empty(t, ti1.Expansions)
	require.False(t, g.Reachable(5))

	// A repeat build produces an identical, stable result.
	repeat, err := BuildReachability(m, []wasm.Index{1})
	require.NoError(t, err)
	require.Equal(t, g, repeat)

	// Reachable table.fill and table.grow also make a table unknown, and the
	// expansion records the writing function and instruction offset.
	writers := &wasm.Module{
		TypeSection:     []*wasm.FunctionType{{}},
		FunctionSection: []wasm.Index{0, 0, 0},
		TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 1}, {Type: wasm.RefTypeFuncref, Min: 1}},
		CodeSection: []*wasm.Code{
			// root calls both writers directly
			{Body: []byte{wasm.OpcodeCall, 0x01, wasm.OpcodeCall, 0x02, wasm.OpcodeEnd}},
			// i32.const 0; ref.null funcref; i32.const 1; table.fill 0
			{Body: []byte{
				wasm.OpcodeI32Const, 0x00, wasm.OpcodeRefNull, wasm.RefTypeFuncref,
				wasm.OpcodeI32Const, 0x01,
				wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableFill, 0x00,
				wasm.OpcodeEnd,
			}},
			// ref.null funcref; i32.const 1; table.grow 1
			{Body: []byte{
				wasm.OpcodeRefNull, wasm.RefTypeFuncref, wasm.OpcodeI32Const, 0x01,
				wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableGrow, 0x01,
				wasm.OpcodeEnd,
			}},
		},
	}
	wg, err := BuildReachability(writers, []wasm.Index{0})
	require.NoError(t, err)

	ft := tableInfo(wg, 0)
	require.True(t, ft.Unknown)
	require.True(t, ft.PossibleExternal)
	var fillExp *TableExpansion
	for i := range ft.Expansions {
		if ft.Expansions[i].Kind == ExpansionTableFill {
			fillExp = &ft.Expansions[i]
		}
	}
	require.NotNil(t, fillExp)
	require.Equal(t, wasm.Index(1), fillExp.Writer)
	require.Equal(t, 6, fillExp.Offset)

	gt := tableInfo(wg, 1)
	require.True(t, gt.Unknown)
	require.True(t, gt.PossibleExternal)
	var growExp *TableExpansion
	for i := range gt.Expansions {
		if gt.Expansions[i].Kind == ExpansionTableGrow {
			growExp = &gt.Expansions[i]
		}
	}
	require.NotNil(t, growExp)
	require.Equal(t, wasm.Index(2), growExp.Writer)
	require.Equal(t, 4, growExp.Offset)
}

// testReachabilityCopy covers candidate propagation between concrete tables,
// unknown propagation, and copy-caused reachability.
func testReachabilityCopy(t *testing.T) {
	// Concrete tables: table.copy dst 1 src 0 moves f2 into table 1, where
	// the call_indirect now sees both f2 and the original f3.
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{}, // type 0
			{Params: []wasm.ValueType{wasm.ValueTypeI32}}, // type 1
		},
		FunctionSection: []wasm.Index{0, 1, 0, 0, 0}, // local 0 root, 1 distractor, 2, 3, 4
		TableSection: []*wasm.Table{
			{Type: wasm.RefTypeFuncref, Min: 2},
			{Type: wasm.RefTypeFuncref, Min: 2},
		},
		ElementSection: []*wasm.ElementSegment{
			activeSegment(0, 0, []*wasm.Index{indexPtr(2)}),
			activeSegment(1, 0, []*wasm.Index{indexPtr(3)}),
		},
		CodeSection: []*wasm.Code{
			// table.copy 1 0; i32.const 0; call_indirect type 0 table 1
			{Body: []byte{
				wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableCopy, 0x01, 0x00,
				wasm.OpcodeI32Const, 0x00,
				wasm.OpcodeCallIndirect, 0x00, 0x01,
				wasm.OpcodeEnd,
			}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
		},
	}

	g, err := BuildReachability(m, []wasm.Index{0})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{0, 2, 3}, reachableIndices(g))
	require.False(t, g.Reachable(1))
	require.False(t, g.Reachable(4), "concrete tables must not add all same-signature functions")

	// table.copy is at offset 0; call_indirect follows at offset 6.
	require.Equal(t, []wasm.Index{2, 3}, callTargets(g, 0, 6))

	require.Equal(t, []wasm.Index{2}, tableInfo(g, 0).Candidates)
	require.Equal(t, []wasm.Index{2, 3}, tableInfo(g, 1).Candidates)
	require.False(t, tableInfo(g, 1).Unknown)

	var copyExp *TableExpansion
	for i := range tableInfo(g, 1).Expansions {
		e := &tableInfo(g, 1).Expansions[i]
		if e.Kind == ExpansionTableCopy {
			copyExp = e
		}
	}
	require.NotNil(t, copyExp)
	require.Equal(t, wasm.Index(0), copyExp.Writer)
	require.Equal(t, 0, copyExp.Offset)
	require.True(t, copyExp.HasSourceTable)
	require.Equal(t, wasm.Index(0), copyExp.SourceTable)

	// Unknown propagation: a reachable table.set makes table 0 unknown; a
	// subsequent table.copy then makes table 1 unknown as well.
	u := &wasm.Module{
		TypeSection:     []*wasm.FunctionType{{}},
		FunctionSection: []wasm.Index{0, 0},
		TableSection: []*wasm.Table{
			{Type: wasm.RefTypeFuncref, Min: 1},
			{Type: wasm.RefTypeFuncref, Min: 1},
		},
		CodeSection: []*wasm.Code{
			// call 1; table.copy dst 1 src 0
			{Body: []byte{wasm.OpcodeCall, 0x01, wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableCopy, 0x01, 0x00, wasm.OpcodeEnd}},
			// table.set 0
			{Body: []byte{
				wasm.OpcodeI32Const, 0x00, wasm.OpcodeRefNull, wasm.RefTypeFuncref,
				wasm.OpcodeTableSet, 0x00, wasm.OpcodeEnd,
			}},
		},
	}
	ug, err := BuildReachability(u, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, tableInfo(ug, 0).Unknown)
	require.True(t, tableInfo(ug, 1).Unknown)
	require.True(t, tableInfo(ug, 1).PossibleExternal)
	var unknownCopy *TableExpansion
	for i := range tableInfo(ug, 1).Expansions {
		e := &tableInfo(ug, 1).Expansions[i]
		if e.Kind == ExpansionTableCopy {
			unknownCopy = e
		}
	}
	require.NotNil(t, unknownCopy)
	require.Equal(t, wasm.Index(0), unknownCopy.Writer)
	require.Equal(t, 2, unknownCopy.Offset)
	require.True(t, unknownCopy.HasSourceTable)
	require.Equal(t, wasm.Index(0), unknownCopy.SourceTable)

	t.Run("table-init and elem-drop", func(t *testing.T) {
		initM := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0, 0, 0}, // local 0 root, 1, 2
			TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 3}},
			ElementSection: []*wasm.ElementSegment{
				// passive segments do not initialize the table at instantiation
				{Type: wasm.RefTypeFuncref, Mode: wasm.ElementModePassive, Init: []*wasm.Index{indexPtr(1), nil, indexPtr(2)}},
			},
			CodeSection: []*wasm.Code{
				{Body: []byte{
					wasm.OpcodeI32Const, 0x00, // d
					wasm.OpcodeI32Const, 0x00, // s
					wasm.OpcodeI32Const, 0x03, // n
					wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableInit, 0x00, 0x00, // elem 0, table 0
					wasm.OpcodeMiscPrefix, wasm.OpcodeMiscElemDrop, 0x00, // elem.drop 0
					wasm.OpcodeI32Const, 0x00,
					wasm.OpcodeCallIndirect, 0x00, 0x00,
					wasm.OpcodeEnd,
				}},
				{Body: []byte{wasm.OpcodeEnd}},
				{Body: []byte{wasm.OpcodeEnd}},
			},
		}
		g, err := BuildReachability(initM, []wasm.Index{0})
		require.NoError(t, err)
		// table.init adds the passive segment's non-null refs; elem.drop does
		// not shrink the path-insensitive candidate set.
		require.Equal(t, []wasm.Index{0, 1, 2}, reachableIndices(g))
		require.Equal(t, []wasm.Index{1, 2}, callTargets(g, 0, 15))
		require.Equal(t, []wasm.Index{1, 2}, tableInfo(g, 0).Candidates)

		var initExp *TableExpansion
		for i := range tableInfo(g, 0).Expansions {
			e := &tableInfo(g, 0).Expansions[i]
			if e.Kind == ExpansionTableInit {
				initExp = e
			}
		}
		require.NotNil(t, initExp, "table.init must be traced")
		require.True(t, initExp.HasWriter)
		require.Equal(t, wasm.Index(0), initExp.Writer)
		require.Equal(t, 6, initExp.Offset)
		require.Equal(t, 0, initExp.SegmentIndex)
	})
}

// testReachabilitySignature covers matching a call_indirect type against a
// candidate with the same parameters/results but a different type index, while
// excluding structurally different signatures.
func testReachabilitySignature(t *testing.T) {
	sig := &wasm.FunctionType{
		Params:  []wasm.ValueType{wasm.ValueTypeI32, wasm.ValueTypeI64},
		Results: []wasm.ValueType{wasm.ValueTypeF32},
	}
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			sig, // type 0
			{Params: []wasm.ValueType{wasm.ValueTypeI32, wasm.ValueTypeI64}, Results: []wasm.ValueType{wasm.ValueTypeF32}}, // type 1, same shape
			{Params: []wasm.ValueType{wasm.ValueTypeI32}, Results: []wasm.ValueType{wasm.ValueTypeF32}},                    // type 2, different
		},
		FunctionSection: []wasm.Index{0, 1, 2, 0}, // local 0 root(type0), 1(type1), 2(type2), 3(type0)
		TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 3}},
		ElementSection: []*wasm.ElementSegment{
			activeSegment(0, 0, []*wasm.Index{indexPtr(1), indexPtr(2), indexPtr(3)}),
		},
		CodeSection: []*wasm.Code{
			{Body: []byte{wasm.OpcodeI32Const, 0x00, wasm.OpcodeCallIndirect, 0x00, 0x00, wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
		},
	}

	g, err := BuildReachability(m, []wasm.Index{0})
	require.NoError(t, err)
	// f1 has a different type index but the same signature, so it matches.
	// f2's signature differs, so it does not.
	require.Equal(t, []wasm.Index{0, 1, 3}, reachableIndices(g))
	require.False(t, g.Reachable(2))
	require.Equal(t, []wasm.Index{1, 3}, callTargets(g, 0, 2))
	for _, s := range callSites(g, 0, 2) {
		require.Equal(t, CallIndirect, s.Kind)
		require.Equal(t, wasm.Index(0), s.TypeIndex)
		require.Equal(t, wasm.Index(0), s.TableIndex)
	}
}

// testReachabilityExternal covers imported/exported funcref tables being
// unknown from the start, module function enumeration with matching signatures,
// the possible-external marker, and non-function imports not consuming
// function indices.
func testReachabilityExternal(t *testing.T) {
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{}, // type 0
			{Params: []wasm.ValueType{wasm.ValueTypeI32}}, // type 1
		},
		ImportSection: []*wasm.Import{
			{Module: "host", Name: "fn", Type: wasm.ExternTypeFunc, DescFunc: 0}, // function index 0
			{Module: "host", Name: "tbl", Type: wasm.ExternTypeTable, DescTable: &wasm.Table{Type: wasm.RefTypeFuncref, Min: 1}},
			{Module: "host", Name: "g", Type: wasm.ExternTypeGlobal, DescGlobal: &wasm.GlobalType{ValType: wasm.ValueTypeI32}},
			{Module: "host", Name: "mem", Type: wasm.ExternTypeMemory, DescMem: &wasm.Memory{Min: 1}},
		},
		// The table import is table 0; the defined table is table 1.
		// The function import is function 0; local functions start at 1.
		FunctionSection: []wasm.Index{0, 1, 0}, // local 1 root(type0), 2(type1), 3(type0)
		TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 2}},
		ExportSection: []*wasm.Export{
			{Name: "tbl", Type: wasm.ExternTypeTable, Index: 1},
		},
		CodeSection: []*wasm.Code{
			// call_indirect type 0 table 0; call_indirect type 0 table 1
			{Body: []byte{
				wasm.OpcodeI32Const, 0x00,
				wasm.OpcodeCallIndirect, 0x00, 0x00,
				wasm.OpcodeCallIndirect, 0x00, 0x01,
				wasm.OpcodeEnd,
			}},
			{Body: []byte{wasm.OpcodeEnd}},
			{Body: []byte{wasm.OpcodeEnd}},
		},
	}

	g, err := BuildReachability(m, []wasm.Index{1})
	require.NoError(t, err)

	// Both tables are unknown, so every module function with type 0 is a
	// candidate: imported function 0, root 1, and local 3. Local 2 has type 1.
	require.Equal(t, []wasm.Index{0, 1, 3}, reachableIndices(g))
	require.False(t, g.Reachable(2))
	require.Equal(t, []wasm.Index{1, 0}, g.CallPath(0))

	require.Equal(t, []wasm.Index{0, 1, 3}, callTargets(g, 1, 2))
	require.Equal(t, []wasm.Index{0, 1, 3}, callTargets(g, 1, 5))
	for _, s := range append(callSites(g, 1, 2), callSites(g, 1, 5)...) {
		require.Equal(t, CallIndirect, s.Kind)
		require.Equal(t, wasm.Index(0), s.TypeIndex)
		require.NotEmpty(t, s.UnknownReason)
		require.True(t, s.PossibleExternal)
	}
	for _, s := range callSites(g, 1, 2) {
		require.Equal(t, wasm.Index(0), s.TableIndex)
	}
	for _, s := range callSites(g, 1, 5) {
		require.Equal(t, wasm.Index(1), s.TableIndex)
	}

	for _, idx := range []wasm.Index{0, 1} {
		ti := tableInfo(g, idx)
		require.NotNil(t, ti)
		require.True(t, ti.Unknown)
		require.True(t, ti.PossibleExternal)
		require.Empty(t, ti.Candidates)
		require.NotEmpty(t, ti.UnknownReasons)
	}

	// The imported host function itself is reachable and marked imported.
	for _, f := range g.Functions {
		if f.Index == 0 {
			require.True(t, f.Imported)
		}
	}
}

// testReachabilityMalformed covers truncated immediates, out-of-range indices,
// and unsupported encodings, each reporting a function index and a byte offset.
func testReachabilityMalformed(t *testing.T) {
	oneFunc := func(body []byte) *wasm.Module {
		return &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0},
			CodeSection:     []*wasm.Code{{Body: body}},
		}
	}

	t.Run("truncated immediate", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{wasm.OpcodeI32Const}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 0, "truncated")
	})

	t.Run("truncated call_indirect table", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{
			wasm.OpcodeI32Const, 0x00, wasm.OpcodeCallIndirect, 0x00,
		}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 2, "truncated")
	})

	t.Run("out-of-range call", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{
			wasm.OpcodeNop, wasm.OpcodeCall, 0x01, wasm.OpcodeEnd,
		}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 1, "out of range")
	})

	t.Run("out-of-range table", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{
			wasm.OpcodeI32Const, 0x00, wasm.OpcodeRefNull, wasm.RefTypeFuncref,
			wasm.OpcodeTableSet, 0x05, wasm.OpcodeEnd,
		}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 4, "out of range")
	})

	t.Run("SIMD is unsupported", func(t *testing.T) {
		// A v128.const suffix followed by 16 immediate bytes. A blind byte
		// scan would see 0x10 here and fabricate a call, so the scanner must
		// reject the vector opcode stream outright.
		body := []byte{wasm.OpcodeVecPrefix, wasm.OpcodeVecV128Const,
			0x10, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
			0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00, wasm.OpcodeEnd}
		g, err := BuildReachability(oneFunc(body), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 0, "SIMD")
	})

	t.Run("threads are unsupported", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{0xfe, 0x00, wasm.OpcodeEnd}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 0, "threads")
	})

	t.Run("exceptions are unsupported", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{0x06, wasm.OpcodeEnd}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 0, "exception")
	})

	t.Run("unsupported misc opcode", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{wasm.OpcodeMiscPrefix, 0x7f, wasm.OpcodeEnd}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 0, "unsupported misc")
	})

	t.Run("element function index out of range", func(t *testing.T) {
		m := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0},
			TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 1}},
			ElementSection:  []*wasm.ElementSegment{activeSegment(0, 0, []*wasm.Index{indexPtr(99)})},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
		}
		g, err := BuildReachability(m, []wasm.Index{0})
		require.Nil(t, g)
		requireSegmentError(t, err, 0, "out of range")
	})

	t.Run("active segment out of bounds", func(t *testing.T) {
		m := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0},
			TableSection:    []*wasm.Table{{Type: wasm.RefTypeFuncref, Min: 1}},
			ElementSection:  []*wasm.ElementSegment{activeSegment(0, 0, []*wasm.Index{indexPtr(0), indexPtr(0)})},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
		}
		g, err := BuildReachability(m, []wasm.Index{0})
		require.Nil(t, g)
		requireSegmentError(t, err, 0, "beyond table")
	})

	t.Run("missing end opcode", func(t *testing.T) {
		g, err := BuildReachability(oneFunc([]byte{wasm.OpcodeNop}), []wasm.Index{0})
		require.Nil(t, g)
		requireFunctionError(t, err, 0, 1, "missing end opcode")
	})
}
