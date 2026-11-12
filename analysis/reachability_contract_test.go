package analysis

import (
	"testing"

	"github.com/stretchr/testify/require"

	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// ---- small module construction helpers ------------------------------------

func u32p(v uint32) *uint32 { return &v }

func i32ConstExpr(v int32) *wasm.ConstantExpression {
	return &wasm.ConstantExpression{Opcode: wasm.OpcodeI32Const, Data: leb128.EncodeInt32(v)}
}

func globalGetExpr(g uint32) *wasm.ConstantExpression {
	return &wasm.ConstantExpression{Opcode: wasm.OpcodeGlobalGet, Data: leb128.EncodeUint32(g)}
}

func activeElem(table wasm.Index, offset int32, refs ...*wasm.Index) *wasm.ElementSegment {
	return &wasm.ElementSegment{
		Mode:       wasm.ElementModeActive,
		TableIndex: table,
		OffsetExpr: i32ConstExpr(offset),
		Init:       refs,
		Type:       wasm.RefTypeFuncref,
	}
}

func passiveElem(refs ...*wasm.Index) *wasm.ElementSegment {
	return &wasm.ElementSegment{Mode: wasm.ElementModePassive, Init: refs, Type: wasm.RefTypeFuncref}
}

func declarativeElem(refs ...*wasm.Index) *wasm.ElementSegment {
	return &wasm.ElementSegment{Mode: wasm.ElementModeDeclarative, Init: refs, Type: wasm.RefTypeFuncref}
}

type moduleSpec struct {
	types     []*wasm.FunctionType
	imports   []*wasm.Import
	funcTypes []wasm.Index
	bodies    [][]byte
	tables    []*wasm.Table
	elems     []*wasm.ElementSegment
	exports   []*wasm.Export
}

func buildModule(t *testing.T, s moduleSpec) *wasm.Module {
	t.Helper()
	require.Equal(t, len(s.funcTypes), len(s.bodies), "funcTypes and bodies must be index-correlated")
	codes := make([]*wasm.Code, len(s.bodies))
	for i, body := range s.bodies {
		codes[i] = &wasm.Code{Body: body}
	}
	return &wasm.Module{
		TypeSection:     s.types,
		ImportSection:   s.imports,
		FunctionSection: s.funcTypes,
		TableSection:    s.tables,
		ElementSection:  s.elems,
		ExportSection:   s.exports,
		CodeSection:     codes,
	}
}

// edgeKey is the identity used in assertion sets.
type edgeKey struct {
	caller wasm.Index
	offset uint32
	target wasm.Index
	kind   EdgeKind
	table  wasm.Index
}

func edgeKeys(r *Reachability) map[edgeKey]CallEdge {
	out := map[edgeKey]CallEdge{}
	for _, e := range r.Edges() {
		out[edgeKey{e.Caller, e.CallSiteOffset, e.Target, e.Kind, e.Table}] = e
	}
	return out
}

func indirectBody(typeIdx, tableIdx uint32) []byte {
	return []byte{
		wasm.OpcodeCallIndirect,
		encodeSmallULEB(typeIdx),
		encodeSmallULEB(tableIdx),
		wasm.OpcodeEnd,
	}
}

// encodeSmallULEB covers the single-byte indices used by the hand-written
// test bodies; larger indices are encoded through leb128 at the call site.
func encodeSmallULEB(v uint32) byte {
	b := leb128.EncodeUint32(v)
	if len(b) != 1 {
		panic("test helper only supports single-byte indices")
	}
	return b[0]
}

// ---- the contract test -----------------------------------------------------

func TestReachabilityContract(t *testing.T) {
	t.Run("initialization", testInitialization)
	t.Run("fixedpoint", testFixedpoint)
	t.Run("copy", testCopy)
	t.Run("signature", testSignature)
	t.Run("external", testExternal)
	t.Run("malformed", testMalformed)
}

// testInitialization covers active segment application order, later writes
// overwriting earlier ones (including null erasure), passive/declarative
// segments staying out of the table, sparse tables with a large Min and the
// basic edge/path reporting.
func testInitialization(t *testing.T) {
	// t0: () -> ()
	voidType := &wasm.FunctionType{}
	// f0 root: call_indirect through table 0 at body offset 0
	// f1, f2, f3, f4, f5: leaf functions
	m := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0, 0, 0, 0, 0},
		bodies: [][]byte{
			indirectBody(0, 0),
			{wasm.OpcodeEnd},
			{wasm.OpcodeEnd},
			{wasm.OpcodeEnd},
			{wasm.OpcodeEnd},
			{wasm.OpcodeEnd},
		},
		tables: []*wasm.Table{{Min: 4, Type: wasm.RefTypeFuncref}},
		elems: []*wasm.ElementSegment{
			// seg 0: slots 0..1 = f1, f2
			activeElem(0, 0, u32p(1), u32p(2)),
			// seg 1: slot 1 erased to null, slot 2 = f3
			activeElem(0, 1, nil, u32p(3)),
			// seg 2 passive and seg 3 declarative must not enter the table.
			passiveElem(u32p(4)),
			declarativeElem(u32p(5)),
		},
	})

	r, err := BuildReachability(m, []wasm.Index{0})
	require.NoError(t, err)

	require.Equal(t, []wasm.Index{0, 1, 3}, r.ReachableFunctions(),
		"f2 was overwritten by null; passive f4 and declarative f5 never enter the table")
	require.False(t, r.IsReachable(2))
	require.False(t, r.IsReachable(4))
	require.False(t, r.IsReachable(5))
	require.False(t, r.IsTableUnknown(0))
	require.Equal(t, []wasm.Index{1, 3}, r.TableCandidates(0))

	edges := edgeKeys(r)
	require.Len(t, edges, 2)
	e1, ok := edges[edgeKey{caller: 0, offset: 0, target: 1, kind: EdgeKindIndirect, table: 0}]
	require.True(t, ok)
	require.False(t, e1.MayCallExternal)
	_, ok = edges[edgeKey{0, 0, 3, EdgeKindIndirect, 0}]
	require.True(t, ok)

	// Every reachable function must have a traceable root path.
	path := r.PathTo(3)
	require.Equal(t, []PathStep{
		{Function: 0},
		{Function: 3, CallSiteOffset: 0, Kind: EdgeKindIndirect, Table: 0},
	}, path)
	require.Equal(t, []PathStep{{Function: 0}}, r.PathTo(0))
	require.Nil(t, r.PathTo(4))

	// Origins trace back to the active segment that last wrote each slot.
	origins := r.TableCandidateOrigins(0)
	require.Contains(t, origins, TableCandidateOrigin{
		Kind: TableOriginActiveElement, SegmentIndex: 0,
	}) // f1 survives from seg 0
	require.Contains(t, origins, TableCandidateOrigin{
		Kind: TableOriginActiveElement, SegmentIndex: 1,
	}) // f3 comes from seg 1

	// Returned slices are independent copies.
	cands := r.TableCandidates(0)
	cands[0] = 99
	require.Equal(t, []wasm.Index{1, 3}, r.TableCandidates(0))

	// A huge Min must not be materialized as an array: writing near the end
	// still works, writing past Min is rejected.
	big := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0},
		bodies:    [][]byte{indirectBody(0, 0), {wasm.OpcodeEnd}},
		tables:    []*wasm.Table{{Min: 10_000_000, Type: wasm.RefTypeFuncref}},
		elems:     []*wasm.ElementSegment{activeElem(0, 9_999_999, u32p(1))},
	})
	rBig, err := BuildReachability(big, []wasm.Index{0})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{0, 1}, rBig.ReachableFunctions())

	overflow := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0},
		bodies:    [][]byte{indirectBody(0, 0), {wasm.OpcodeEnd}},
		tables:    []*wasm.Table{{Min: 1, Type: wasm.RefTypeFuncref}},
		elems:     []*wasm.ElementSegment{activeElem(0, 0, u32p(1), u32p(1))},
	})
	_, err = BuildReachability(overflow, []wasm.Index{0})
	require.Error(t, err)
	require.Contains(t, err.Error(), "out of bounds")
}

// testFixedpoint covers the central iteration requirement: a writer found
// indirectly adds candidates to an existing call site, while a writer that is
// itself unreachable must not pollute the table summaries.
func testFixedpoint(t *testing.T) {
	voidType := &wasm.FunctionType{}
	// f0 root: call_indirect table 0 (offset 0)
	// f1 leaf
	// f2 reachable writer: table.init seg 0 into table 0
	// f3 leaf, only referenced from the passive segment used by f2
	// f4 unreachable writer: table.set of f5 — must never take effect
	// f5 leaf, only reachable if f4's write were counted
	tableInitBody := []byte{
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableInit,
		0x00, // element segment 0
		0x00, // table 0
		wasm.OpcodeEnd,
	}
	unreachableWriterBody := []byte{
		wasm.OpcodeI32Const, 0x00,
		wasm.OpcodeRefFunc, 0x05,
		wasm.OpcodeTableSet, 0x00,
		wasm.OpcodeEnd,
	}
	m := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0, 0, 0, 0, 0},
		bodies: [][]byte{
			indirectBody(0, 0),    // f0
			{wasm.OpcodeEnd},      // f1
			tableInitBody,         // f2
			{wasm.OpcodeEnd},      // f3
			unreachableWriterBody, // f4
			{wasm.OpcodeEnd},      // f5
		},
		tables: []*wasm.Table{{Min: 10, Type: wasm.RefTypeFuncref}},
		elems: []*wasm.ElementSegment{
			activeElem(0, 0, u32p(2)), // table starts with only f2
			passiveElem(u32p(3)),      // seg 1? no: this is segment index 1
		},
	})
	// The passive segment is the second element entry, hence index 1; point
	// f2's table.init at it explicitly.
	m.CodeSection[2].Body = []byte{
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableInit,
		0x01, // element segment 1
		0x00, // table 0
		wasm.OpcodeEnd,
	}

	r, err := BuildReachability(m, []wasm.Index{0})
	require.NoError(t, err)

	// Fixed point: f0 -> f2 via the active slot, f2's table.init adds f3, and
	// f0's pre-existing call site is re-expanded to pick f3 up.
	require.Equal(t, []wasm.Index{0, 2, 3}, r.ReachableFunctions())
	require.False(t, r.IsReachable(1), "f1 is nowhere referenced")
	require.False(t, r.IsReachable(4), "unreachable writer must not be analyzed")
	require.False(t, r.IsReachable(5), "unreachable writer's target must not leak into the table")
	require.False(t, r.IsTableUnknown(0), "table.init keeps the table concrete")

	edges := edgeKeys(r)
	_, ok := edges[edgeKey{0, 0, 2, EdgeKindIndirect, 0}]
	require.True(t, ok, "initial active-slot target")
	_, ok = edges[edgeKey{0, 0, 3, EdgeKindIndirect, 0}]
	require.True(t, ok, "target added after the indirectly-found writer ran")
	require.Len(t, edges, 2)

	require.Contains(t, r.TableCandidateOrigins(0), TableCandidateOrigin{
		Kind: TableOriginTableInit, SegmentIndex: 1, Function: 2, BodyOffset: 0,
	})
	require.Equal(t, []wasm.Index{2, 3}, r.TableCandidates(0))

	// Paths remain reconstructable after the fixed point is reached.
	require.Equal(t, []PathStep{
		{Function: 0},
		{Function: 3, CallSiteOffset: 0, Kind: EdgeKindIndirect, Table: 0},
	}, r.PathTo(3))

	// When a reachable writer turns a previously concrete table unknown, the
	// existing call site is re-expanded: the old edge is upgraded (not
	// duplicated) and every compatible module function becomes a target.
	setUnknownBody := []byte{
		wasm.OpcodeI32Const, 0x00,
		wasm.OpcodeRefFunc, 0x01,
		wasm.OpcodeTableSet, 0x00, // offset 4
		wasm.OpcodeEnd,
	}
	modUnknown := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0, 0},
		bodies: [][]byte{
			indirectBody(0, 0), // f0 root, concrete site initially
			setUnknownBody,     // f1 reachable writer making table 0 unknown
			{wasm.OpcodeEnd},   // f2 only reachable once the table is unknown
		},
		tables: []*wasm.Table{{Min: 4, Type: wasm.RefTypeFuncref}},
		elems:  []*wasm.ElementSegment{activeElem(0, 0, u32p(1))},
	})
	rUnk, err := BuildReachability(modUnknown, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, rUnk.IsTableUnknown(0))
	require.Equal(t, []wasm.Index{0, 1, 2}, rUnk.ReachableFunctions())
	unkEdges := edgeKeys(rUnk)
	// Unknown tables may hold any compatible module function, including f0
	// itself. Each (site, target) still appears exactly once: the f1 edge got
	// upgraded in place rather than duplicated.
	require.Len(t, unkEdges, 3)
	require.True(t, unkEdges[edgeKey{0, 0, 1, EdgeKindIndirect, 0}].MayCallExternal)
	require.True(t, unkEdges[edgeKey{0, 0, 0, EdgeKindIndirect, 0}].MayCallExternal)
}

// testCopy covers candidate and unknown propagation between tables through
// reachable table.copy.
func testCopy(t *testing.T) {
	voidType := &wasm.FunctionType{}
	// f0 root: table.copy 1 <- 0, then call_indirect through table 1.
	// f1, f2 leaves placed in table 0.
	copyThenCall := []byte{
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableCopy,
		0x01, // destination table 1
		0x00, // source table 0
		wasm.OpcodeCallIndirect, 0x00, 0x01,
		wasm.OpcodeEnd,
	}
	m := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0, 0},
		bodies: [][]byte{
			copyThenCall,
			{wasm.OpcodeEnd},
			{wasm.OpcodeEnd},
		},
		tables: []*wasm.Table{
			{Min: 4, Type: wasm.RefTypeFuncref},
			{Min: 4, Type: wasm.RefTypeFuncref},
		},
		elems: []*wasm.ElementSegment{activeElem(0, 0, u32p(1), u32p(2))},
	})

	r, err := BuildReachability(m, []wasm.Index{0})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{0, 1, 2}, r.ReachableFunctions())
	require.Equal(t, []wasm.Index{1, 2}, r.TableCandidates(1),
		"table.copy propagates the source candidates to the destination")
	require.Contains(t, r.TableCandidateOrigins(1), TableCandidateOrigin{
		Kind: TableOriginTableCopy, Function: 0, BodyOffset: 0,
	})
	edges := edgeKeys(r)
	// table.copy occupies bytes 0..3; the call_indirect starts at offset 4.
	_, ok := edges[edgeKey{0, 4, 1, EdgeKindIndirect, 1}]
	require.True(t, ok)
	_, ok = edges[edgeKey{0, 4, 2, EdgeKindIndirect, 1}]
	require.True(t, ok)

	// Unknown state propagates as well: an exported source table makes the
	// destination unknown after the copy.
	m2 := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0},
		bodies:    [][]byte{copyThenCall, {wasm.OpcodeEnd}},
		tables: []*wasm.Table{
			{Min: 4, Type: wasm.RefTypeFuncref},
			{Min: 4, Type: wasm.RefTypeFuncref},
		},
		exports: []*wasm.Export{{Name: "t0", Type: wasm.ExternTypeTable, Index: 0}},
	})
	r2, err := BuildReachability(m2, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, r2.IsTableUnknown(1))
	require.Contains(t, r2.TableUnknownOrigins(1), TableUnknownOrigin{
		Reason: UnknownReasonCopied, SegmentIndex: -1, Function: 0, BodyOffset: 0, SourceTable: 0,
	})
	edge, ok := edgeKeys(r2)[edgeKey{0, 4, 1, EdgeKindIndirect, 1}]
	require.True(t, ok)
	require.True(t, edge.MayCallExternal, "unknown destination may call external functions")

	// A source table that grows after the copy was processed must still feed
	// the destination, including through a transitive copy chain.
	rootWithChain := []byte{
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableCopy, 0x01, 0x00, // table 1 <- table 0
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableCopy, 0x02, 0x01, // table 2 <- table 1
		wasm.OpcodeCall, 0x01, // call f1 (the later writer)
		wasm.OpcodeCallIndirect, 0x00, 0x01, // call_indirect table 1
		wasm.OpcodeEnd,
	}
	writerInit := []byte{
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscTableInit,
		0x01, // passive element segment 1
		0x00, // table 0
		wasm.OpcodeEnd,
	}
	m3 := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0, 0},
		bodies: [][]byte{
			rootWithChain,    // f0 root: copies happen before f1 grows the source
			writerInit,       // f1: table.init adds f2 into table 0
			{wasm.OpcodeEnd}, // f2 leaf
		},
		tables: []*wasm.Table{
			{Min: 4, Type: wasm.RefTypeFuncref},
			{Min: 4, Type: wasm.RefTypeFuncref},
			{Min: 4, Type: wasm.RefTypeFuncref},
		},
		elems: []*wasm.ElementSegment{
			activeElem(0, 0, u32p(1)), // seg 0: table 0 initially holds f1
			passiveElem(u32p(2)),      // seg 1: f2, only inserted by f1's table.init
		},
	})
	r3, err := BuildReachability(m3, []wasm.Index{0})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{0, 1, 2}, r3.ReachableFunctions())
	require.Equal(t, []wasm.Index{1, 2}, r3.TableCandidates(1),
		"the destination keeps receiving source-table additions after the copy")
	require.Equal(t, []wasm.Index{1, 2}, r3.TableCandidates(2),
		"the copy relation replays transitively across table 2 <- table 1 <- table 0")
	// call_indirect starts at byte 10 and must pick f2 up through table 1.
	_, ok = edgeKeys(r3)[edgeKey{0, 10, 2, EdgeKindIndirect, 1}]
	require.True(t, ok, "the pre-existing indirect site re-expands after the delayed source growth")
}

// testSignature covers structural call_indirect matching across distinct type
// indices, including imported functions.
func testSignature(t *testing.T) {
	i32 := wasm.ValueTypeI32
	// t0 and t1 are structurally identical but distinct type indices;
	// t2 has a different shape.
	t0 := &wasm.FunctionType{Params: []wasm.ValueType{i32}, Results: []wasm.ValueType{i32}}
	t1 := &wasm.FunctionType{Params: []wasm.ValueType{i32}, Results: []wasm.ValueType{i32}}
	t2 := &wasm.FunctionType{Params: []wasm.ValueType{i32}}
	m := buildModule(t, moduleSpec{
		types: []*wasm.FunctionType{t0, t1, t2},
		imports: []*wasm.Import{
			// full function index 0, uses type index 1: same shape as t0.
			{Module: "env", Name: "imp", Type: wasm.ExternTypeFunc, DescFunc: 1},
		},
		// local functions: full indices 1 (t1), 2 (t2)
		funcTypes: []wasm.Index{1, 2},
		bodies: [][]byte{
			indirectBody(0, 0), // f1 root, call_indirect with type use t0
			{wasm.OpcodeEnd},   // f2 wrong signature
		},
		tables: []*wasm.Table{{Min: 3, Type: wasm.RefTypeFuncref}},
		// slot 0: imported f0 (t1 shape); slot 1: local f1 (t1 shape);
		// slot 2: local f2 (t2 shape, must not match the call).
		elems: []*wasm.ElementSegment{activeElem(0, 0, u32p(0), u32p(1), u32p(2))},
	})

	r, err := BuildReachability(m, []wasm.Index{1})
	require.NoError(t, err)

	// The table still enumerates every reference it holds...
	require.Equal(t, []wasm.Index{0, 1, 2}, r.TableCandidates(0))
	// ...but only the structurally matching functions become call targets.
	edges := edgeKeys(r)
	_, ok := edges[edgeKey{1, 0, 0, EdgeKindIndirect, 0}]
	require.True(t, ok, "imported function with an equal-shape but different type index matches")
	_, ok = edges[edgeKey{1, 0, 1, EdgeKindIndirect, 0}]
	require.True(t, ok, "same-shape local function matches")
	_, ok = edges[edgeKey{1, 0, 2, EdgeKindIndirect, 0}]
	require.False(t, ok, "different signature must not match despite living in the table")
	require.Len(t, edges, 2)
	require.Equal(t, []wasm.Index{0, 1}, r.ReachableFunctions())

	// The imported root itself is reachable but has no body to scan.
	rImp, err := BuildReachability(m, []wasm.Index{0})
	require.NoError(t, err)
	require.Equal(t, []wasm.Index{0}, rImp.ReachableFunctions())
}

// testExternal covers the conditions that make a table unknown and the
// traceability of each reason.
func testExternal(t *testing.T) {
	voidType := &wasm.FunctionType{}
	i32Type := &wasm.FunctionType{Params: []wasm.ValueType{wasm.ValueTypeI32}, Results: []wasm.ValueType{wasm.ValueTypeI32}}

	// An imported funcref table is unknown from the start.
	imported := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType, i32Type},
		imports:   []*wasm.Import{{Module: "env", Name: "tab", Type: wasm.ExternTypeTable, DescTable: &wasm.Table{Min: 2, Type: wasm.RefTypeFuncref}}},
		funcTypes: []wasm.Index{0, 0, 1},
		bodies: [][]byte{
			indirectBody(0, 0), // f0 root, call_indirect type t0
			{wasm.OpcodeEnd},   // f1 t0 compatible
			{wasm.OpcodeEnd},   // f2 t1 incompatible
		},
	})
	r, err := BuildReachability(imported, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, r.IsTableUnknown(0))
	require.Contains(t, r.TableUnknownOrigins(0), TableUnknownOrigin{
		Reason: UnknownReasonImportedTable, SegmentIndex: -1,
	})
	// Unknown table: every module-internal compatible function is a candidate,
	// and the edges flag possible external calls.
	require.Equal(t, []wasm.Index{0, 1}, r.ReachableFunctions())
	edge, ok := edgeKeys(r)[edgeKey{0, 0, 1, EdgeKindIndirect, 0}]
	require.True(t, ok)
	require.True(t, edge.MayCallExternal)
	_, ok = edgeKeys(r)[edgeKey{0, 0, 2, EdgeKindIndirect, 0}]
	require.False(t, ok, "incompatible signature stays excluded even when unknown")

	// An exported defined table is unknown for the same reason.
	exported := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0},
		bodies:    [][]byte{{wasm.OpcodeEnd}},
		tables:    []*wasm.Table{{Min: 1, Type: wasm.RefTypeFuncref}},
		exports:   []*wasm.Export{{Name: "tab", Type: wasm.ExternTypeTable, Index: 0}},
	})
	rExp, err := BuildReachability(exported, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, rExp.IsTableUnknown(0))
	require.Contains(t, rExp.TableUnknownOrigins(0), TableUnknownOrigin{Reason: UnknownReasonExportedTable, SegmentIndex: -1})

	// A reachable table.set turns its table unknown, attributed to the exact
	// instruction; elem.drop must not undo anything.
	writerBody := []byte{
		wasm.OpcodeI32Const, 0x00,
		wasm.OpcodeRefFunc, 0x02,
		wasm.OpcodeTableSet, 0x00, // offset 4
		wasm.OpcodeMiscPrefix, wasm.OpcodeMiscElemDrop, 0x00, // offset 7
		wasm.OpcodeEnd,
	}
	setMod := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0, 0},
		bodies: [][]byte{
			indirectBody(0, 0), // f0 root
			writerBody,         // f1 reachable writer
			{wasm.OpcodeEnd},   // f2
		},
		tables: []*wasm.Table{{Min: 4, Type: wasm.RefTypeFuncref}},
		elems:  []*wasm.ElementSegment{activeElem(0, 0, u32p(1)), passiveElem(u32p(2))},
	})
	rSet, err := BuildReachability(setMod, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, rSet.IsTableUnknown(0))
	require.Contains(t, rSet.TableUnknownOrigins(0), TableUnknownOrigin{
		Reason: UnknownReasonReachableTableSet, Function: 1, BodyOffset: 4, SegmentIndex: -1,
	})
	// Once unknown, all compatible module functions become possible targets.
	require.Equal(t, []wasm.Index{0, 1, 2}, rSet.ReachableFunctions())

	// A global.get offset makes the active segment unplaceable.
	globalOff := buildModule(t, moduleSpec{
		types:     []*wasm.FunctionType{voidType},
		funcTypes: []wasm.Index{0, 0},
		bodies:    [][]byte{{wasm.OpcodeEnd}, {wasm.OpcodeEnd}},
		tables:    []*wasm.Table{{Min: 4, Type: wasm.RefTypeFuncref}},
		elems: []*wasm.ElementSegment{{
			Mode:       wasm.ElementModeActive,
			TableIndex: 0,
			OffsetExpr: globalGetExpr(0),
			Init:       []*wasm.Index{u32p(1)},
			Type:       wasm.RefTypeFuncref,
		}},
	})
	rGlob, err := BuildReachability(globalOff, []wasm.Index{0})
	require.NoError(t, err)
	require.True(t, rGlob.IsTableUnknown(0))
	require.Contains(t, rGlob.TableUnknownOrigins(0), TableUnknownOrigin{
		Reason: UnknownReasonActiveOffsetGlobal, SegmentIndex: 0,
	})
}

// testMalformed covers the error contract: every failure names the function
// index and body byte offset, and valid instructions whose immediate byte
// happens to equal 0x10 must not be misread as calls.
func testMalformed(t *testing.T) {
	voidType := &wasm.FunctionType{}

	scenarios := []struct {
		name   string
		body   []byte
		offset uint32
	}{
		{"truncated i32.const immediate", []byte{wasm.OpcodeI32Const}, 0},
		{"truncated br_table labels", []byte{wasm.OpcodeBrTable, 0x02, 0x00}, 0},
		{"truncated block type", []byte{wasm.OpcodeBlock, 0x80}, 0},
		{"truncated misc immediate", []byte{wasm.OpcodeMiscPrefix}, 0},
		{"unsupported SIMD prefix", []byte{wasm.OpcodeVecPrefix, 0x00, wasm.OpcodeEnd}, 0},
		{"unsupported threads prefix", []byte{0xfe, 0x00, 0x00, 0x00, wasm.OpcodeEnd}, 0},
		{"unsupported exception handling try", []byte{0x06, 0x40, wasm.OpcodeEnd, wasm.OpcodeEnd}, 0},
	}
	for _, sc := range scenarios {
		sc := sc
		t.Run(sc.name, func(t *testing.T) {
			m := buildModule(t, moduleSpec{
				types:     []*wasm.FunctionType{voidType},
				funcTypes: []wasm.Index{0},
				bodies:    [][]byte{sc.body},
			})
			_, err := BuildReachability(m, []wasm.Index{0})
			require.Error(t, err)
			var se *ScanError
			require.ErrorAs(t, err, &se)
			require.Equal(t, wasm.Index(0), se.Function)
			require.Equal(t, sc.offset, se.Offset)
		})
	}

	t.Run("out of range direct call", func(t *testing.T) {
		m := buildModule(t, moduleSpec{
			types:     []*wasm.FunctionType{voidType},
			funcTypes: []wasm.Index{0},
			bodies:    [][]byte{{wasm.OpcodeCall, 0x99, 0x01, wasm.OpcodeEnd}}, // 99 in LEB128
		})
		_, err := BuildReachability(m, []wasm.Index{0})
		var se *ScanError
		require.ErrorAs(t, err, &se)
		require.Equal(t, wasm.Index(0), se.Function)
		require.Equal(t, uint32(0), se.Offset)
		require.Equal(t, wasm.OpcodeCall, se.Opcode)
	})

	t.Run("out of range indirect table", func(t *testing.T) {
		m := buildModule(t, moduleSpec{
			types:     []*wasm.FunctionType{voidType},
			funcTypes: []wasm.Index{0},
			bodies:    [][]byte{{wasm.OpcodeCallIndirect, 0x00, 0x05, wasm.OpcodeEnd}},
		})
		_, err := BuildReachability(m, []wasm.Index{0})
		var se *ScanError
		require.ErrorAs(t, err, &se)
		require.Equal(t, wasm.Index(0), se.Function)
		require.Equal(t, uint32(0), se.Offset)
		require.Equal(t, wasm.OpcodeCallIndirect, se.Opcode)
	})

	t.Run("root index out of range", func(t *testing.T) {
		m := buildModule(t, moduleSpec{
			types:     []*wasm.FunctionType{voidType},
			funcTypes: []wasm.Index{0},
			bodies:    [][]byte{{wasm.OpcodeEnd}},
		})
		_, err := BuildReachability(m, []wasm.Index{7})
		require.Error(t, err)
	})

	t.Run("malformed body in unreachable function is not reported", func(t *testing.T) {
		m := buildModule(t, moduleSpec{
			types:     []*wasm.FunctionType{voidType},
			funcTypes: []wasm.Index{0, 0},
			bodies: [][]byte{
				{wasm.OpcodeEnd},      // f0 reachable root, valid
				{wasm.OpcodeI32Const}, // f1 unreachable and malformed
			},
		})
		r, err := BuildReachability(m, []wasm.Index{0})
		require.NoError(t, err)
		require.Equal(t, []wasm.Index{0}, r.ReachableFunctions())
	})

	t.Run("call opcode byte inside an immediate is not a call", func(t *testing.T) {
		// A byte-blind scan would treat the 0x10 inside i32.const's immediate
		// as OpcodeCall and invent an edge.
		m := buildModule(t, moduleSpec{
			types:     []*wasm.FunctionType{voidType},
			funcTypes: []wasm.Index{0},
			bodies:    [][]byte{{wasm.OpcodeI32Const, wasm.OpcodeCall, wasm.OpcodeEnd}},
		})
		r, err := BuildReachability(m, []wasm.Index{0})
		require.NoError(t, err)
		require.Empty(t, r.Edges())
		require.Equal(t, []wasm.Index{0}, r.ReachableFunctions())
	})

	t.Run("edges are stably sorted", func(t *testing.T) {
		// f0 calls f2 then f1 then f2 again; dedup plus stable ordering must
		// produce (0,0,f1) before (0,2,f2).
		m := buildModule(t, moduleSpec{
			types:     []*wasm.FunctionType{voidType},
			funcTypes: []wasm.Index{0, 0, 0},
			bodies: [][]byte{
				{wasm.OpcodeCall, 0x02, wasm.OpcodeCall, 0x01, wasm.OpcodeEnd},
				{wasm.OpcodeEnd},
				{wasm.OpcodeEnd},
			},
		})
		r, err := BuildReachability(m, []wasm.Index{0})
		require.NoError(t, err)
		edges := r.Edges()
		require.Len(t, edges, 2)
		// Sorted by caller, then call site offset, then target.
		require.Equal(t, CallEdge{Caller: 0, CallSiteOffset: 0, Target: 2, Kind: EdgeKindDirect}, edges[0])
		require.Equal(t, CallEdge{Caller: 0, CallSiteOffset: 2, Target: 1, Kind: EdgeKindDirect}, edges[1])
	})
}
