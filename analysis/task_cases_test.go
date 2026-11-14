package analysis

import (
	"bytes"
	"fmt"
	"testing"

	"github.com/tetratelabs/wabin/binary"
	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// TaskCases registers the four task test groups consumed by the injected
// runner (task_support/runner.go.in).
var TaskCases = []TaskCase{
	{Name: "multivalue", Run: testMultivalue},
	{Name: "loop", Run: testLoop},
	{Name: "conditional", Run: testConditional},
	{Name: "deadcode", Run: testDeadcode},
}

// bodyBuilder lays out a function body while recording named instruction
// offsets, so assertions can reference stable Body positions.
type bodyBuilder struct {
	buf   []byte
	marks map[string]uint32
}

func newBody() *bodyBuilder { return &bodyBuilder{marks: map[string]uint32{}} }

func (b *bodyBuilder) mark(name string) *bodyBuilder {
	b.marks[name] = uint32(len(b.buf))
	return b
}

func (b *bodyBuilder) op(ops ...wasm.Opcode) *bodyBuilder {
	b.buf = append(b.buf, ops...)
	return b
}

func (b *bodyBuilder) raw(bs ...byte) *bodyBuilder {
	b.buf = append(b.buf, bs...)
	return b
}

func (b *bodyBuilder) u32(v uint32) *bodyBuilder {
	b.buf = append(b.buf, leb128.EncodeUint32(v)...)
	return b
}

func (b *bodyBuilder) i32(v int32) *bodyBuilder {
	b.buf = append(b.buf, leb128.EncodeInt32(v)...)
	return b
}

func (b *bodyBuilder) i64(v int64) *bodyBuilder {
	b.buf = append(b.buf, leb128.EncodeInt64(v)...)
	return b
}

func findValue(g *Graph, kind ValueKind, off uint32, idx uint32) *Value {
	for _, v := range g.Values {
		if v.Kind == kind && v.Offset == off && v.Index == idx {
			return v
		}
	}
	return nil
}

func findEdges(g *Graph, kind EdgeKind, from uint32) []*Edge {
	var out []*Edge
	for _, e := range g.Edges {
		if e.Kind == kind && e.FromOffset == from {
			out = append(out, e)
		}
	}
	return out
}

func findInstr(g *Graph, off uint32) *Instruction {
	for _, i := range g.Instructions {
		if i.Offset == off {
			return i
		}
	}
	return nil
}

// sameValue compares value node identity across two independently owned
// analysis results (the audit re-analyzes functions, so pointers differ).
func sameValue(a, b *Value) bool {
	return a != nil && b != nil && a.Kind == b.Kind && a.Offset == b.Offset && a.Index == b.Index && a.Type == b.Type
}

// inEdges counts the edges whose targets contain v.
func inEdges(g *Graph, v *Value) int {
	n := 0
	for _, e := range g.Edges {
		for _, t := range e.Targets {
			if t == v {
				n++
			}
		}
	}
	return n
}

// checkSorted asserts the graph's stable ordering: values by byte position,
// edges by (byte position, edge kind, selector), instructions by position.
func checkSorted(t *testing.T, g *Graph) {
	t.Helper()
	for i := 1; i < len(g.Values); i++ {
		a, b := g.Values[i-1], g.Values[i]
		ok := a.Offset < b.Offset ||
			(a.Offset == b.Offset && a.Kind < b.Kind) ||
			(a.Offset == b.Offset && a.Kind == b.Kind && a.Index < b.Index)
		TaskCheck(t, ok, fmt.Sprintf("values not sorted at %d: %v then %v", i, a, b))
	}
	for i := 1; i < len(g.Edges); i++ {
		a, b := g.Edges[i-1], g.Edges[i]
		ok := a.FromOffset < b.FromOffset ||
			(a.FromOffset == b.FromOffset && a.Kind < b.Kind) ||
			(a.FromOffset == b.FromOffset && a.Kind == b.Kind && a.Selector < b.Selector)
		TaskCheck(t, ok, fmt.Sprintf("edges not sorted at %d: %+v then %+v", i, a, b))
	}
	for i := 1; i < len(g.Instructions); i++ {
		TaskCheck(t, g.Instructions[i-1].Offset < g.Instructions[i].Offset, "instructions not sorted")
	}
}

// checkErr asserts a positioned analysis error.
func checkErr(t *testing.T, err error, kind ErrorKind, fn wasm.Index) {
	t.Helper()
	e, ok := err.(*Error)
	TaskCheck(t, ok, fmt.Sprintf("want *Error, got %T (%v)", err, err))
	TaskCheck(t, e.Kind == kind, fmt.Sprintf("want kind %d, got %d (%v)", kind, e.Kind, e))
	TaskCheck(t, e.FuncIndex == fn, fmt.Sprintf("want func %d in error, got %d", fn, e.FuncIndex))
}

// testMultivalue covers multi-value block types (signed LEB33 type index),
// parameter and result order, distinct constant nodes, single-in-edge merge
// parameters, multi-byte immediates, module round-trip stability and the
// host capability audit of a call whose arguments flow through a merge.
func testMultivalue(t *testing.T) {
	const i32t, i64t = wasm.ValueTypeI32, wasm.ValueTypeI64
	types := []*wasm.FunctionType{
		{Params: []wasm.ValueType{i32t, i64t}}, // 0: env.log(i32, i64)
	}
	for len(types) < 64 {
		types = append(types, &wasm.FunctionType{})
	}
	// 64: (i32, i64) -> (i32, i64), referenced by a multi-byte LEB33 index.
	types = append(types, &wasm.FunctionType{Params: []wasm.ValueType{i32t, i64t}, Results: []wasm.ValueType{i32t, i64t}})

	b := newBody()
	b.mark("c1a").op(wasm.OpcodeI32Const).i32(1)
	b.mark("c1b").op(wasm.OpcodeI32Const).i32(1) // same value, distinct node
	b.mark("add").op(wasm.OpcodeI32Add)
	b.mark("c2").op(wasm.OpcodeI64Const).i64(624485)   // multi-byte immediate
	b.mark("blk").op(wasm.OpcodeBlock).raw(0xc0, 0x00) // block type 64, 2-byte LEB33
	b.mark("c3").op(wasm.OpcodeI32Const).i32(3)
	b.mark("c4").op(wasm.OpcodeI64Const).i64(-624485) // multi-byte negative
	b.mark("br").op(wasm.OpcodeBr).u32(0)
	b.mark("bend").op(wasm.OpcodeEnd)
	b.mark("call").op(wasm.OpcodeCall).u32(0)
	b.mark("fend").op(wasm.OpcodeEnd)

	m := &wasm.Module{
		TypeSection:     types,
		ImportSection:   []*wasm.Import{{Type: wasm.ExternTypeFunc, Module: "env", Name: "log", DescFunc: 0}},
		FunctionSection: []wasm.Index{1},
		CodeSection:     []*wasm.Code{{Body: b.buf}},
	}
	wireBefore := binary.EncodeModule(m)

	g, err := AnalyzeStackTransfers(m, 1)
	TaskCheck(t, err == nil, fmt.Sprintf("analyze failed: %v", err))
	mk := b.marks

	// The module, its types and the body are unchanged and still round-trip.
	TaskCheck(t, bytes.Equal(binary.EncodeModule(m), wireBefore), "module changed after analysis")
	dec, err := binary.DecodeModule(wireBefore, wasm.CoreFeaturesV2)
	TaskCheck(t, err == nil && bytes.Equal(binary.EncodeModule(dec), wireBefore), "module no longer round-trips")

	// Equal constants are never merged.
	na, nb := findValue(g, ValueInstr, mk["c1a"], 0), findValue(g, ValueInstr, mk["c1b"], 0)
	TaskCheck(t, na != nil && nb != nil && na != nb, "equal constants must stay distinct nodes")

	// Block params and merge results are independent parameter nodes, in
	// declared order, even with a single incoming edge.
	bp0, bp1 := findValue(g, ValueBlockParam, mk["blk"], 0), findValue(g, ValueBlockParam, mk["blk"], 1)
	TaskCheck(t, bp0 != nil && bp0.Type == i32t && bp1 != nil && bp1.Type == i64t, "block params missing or mis-typed")
	br0, br1 := findValue(g, ValueBlockResult, mk["blk"], 0), findValue(g, ValueBlockResult, mk["blk"], 1)
	TaskCheck(t, br0 != nil && br0.Type == i32t && br1 != nil && br1.Type == i64t, "block results missing or mis-typed")
	TaskCheck(t, inEdges(g, br0) == 1 && inEdges(g, br1) == 1, "single-in-edge merge params must be kept")

	// The enter edge carries the multi-value params in declared order.
	enter := findEdges(g, EdgeEnter, mk["blk"])
	TaskCheck(t, len(enter) == 1, "want one enter edge")
	TaskCheck(t, len(enter[0].Args) == 2 && enter[0].Args[0] == findValue(g, ValueInstr, mk["add"], 0) &&
		enter[0].Args[1] == findValue(g, ValueInstr, mk["c2"], 0), "enter args not in declared order")
	TaskCheck(t, enter[0].Targets[0] == bp0 && enter[0].Targets[1] == bp1, "enter targets mismatch")

	// br carries both results, in order, to the merge parameters.
	br := findEdges(g, EdgeBranch, mk["br"])
	TaskCheck(t, len(br) == 1, "want one br edge")
	TaskCheck(t, len(br[0].Args) == 2 && br[0].Args[0] == findValue(g, ValueInstr, mk["c3"], 0) &&
		br[0].Args[1] == findValue(g, ValueInstr, mk["c4"], 0), "br args not in declared order")
	TaskCheck(t, br[0].Targets[0] == br0 && br[0].Targets[1] == br1, "br targets mismatch")

	// The fallthrough after br is dead: no end edge, yet the merge lives.
	TaskCheck(t, len(findEdges(g, EdgeEnd, mk["bend"])) == 0, "dead fallthrough must not emit an end edge")

	// The call consumes the merge results per the real signature.
	call := findInstr(g, mk["call"])
	TaskCheck(t, call != nil && call.Opcode == wasm.OpcodeCall && call.CallTarget == 0, "call instruction missing")
	TaskCheck(t, len(call.Inputs) == 2 && call.Inputs[0] == br0 && call.Inputs[1] == br1, "call inputs mismatch")

	checkSorted(t, g)

	// Host capability audit: the allowed import call is reported with its
	// argument sources traced through the block parameter edges.
	report := AuditHostCapabilities(m, []HostFuncRef{{Module: "env", Name: "log"}})
	TaskCheck(t, len(report.Functions) == 1 && report.Functions[0].Complete, "audit: function incomplete")
	TaskCheck(t, len(report.Calls) == 1, "audit: want one host call")
	c := report.Calls[0]
	TaskCheck(t, c.Module == "env" && c.Name == "log" && c.CallIndex == 0, "audit: wrong callee")
	TaskCheck(t, c.FuncIndex == 1 && c.Offset == mk["call"], "audit: wrong call site")
	TaskCheck(t, c.Allowed, "audit: allowlisted call must be allowed")
	TaskCheck(t, len(c.Args) == 2 && sameValue(c.Args[0], br0) && sameValue(c.Args[1], br1), "audit: wrong args")
	TaskCheck(t, len(c.Sources) == 2, "audit: want two arg sources")
	TaskCheck(t, len(c.Sources[0].Origins) == 1 && sameValue(c.Sources[0].Origins[0], findValue(g, ValueInstr, mk["c3"], 0)),
		"audit: first arg must trace to the i32 const")
	TaskCheck(t, len(c.Sources[1].Origins) == 1 && sameValue(c.Sources[1].Origins[0], findValue(g, ValueInstr, mk["c4"], 0)),
		"audit: second arg must trace to the i64 const")
	TaskCheck(t, !c.Sources[0].External && !c.Sources[1].External, "audit: const origins are not external")
	TaskCheck(t, report.Admissible(), "audit: all calls allowed, plugin must be admissible")
}

// testLoop covers loop headers whose param types differ from their results,
// back edges that reference the predetermined header param identity, br_if
// argument preservation on the fallthrough, and the synthetic function exit.
// The audit traces one call site's argument through distinct incoming edges.
func testLoop(t *testing.T) {
	const i32t, i64t = wasm.ValueTypeI32, wasm.ValueTypeI64
	b := newBody()
	b.mark("lg0").op(wasm.OpcodeLocalGet).u32(0)
	b.mark("loop").op(wasm.OpcodeLoop).u32(1) // (param i64) (result i32)
	b.mark("callLog").op(wasm.OpcodeCall).u32(0)
	b.mark("lgx").op(wasm.OpcodeLocalGet).u32(0)
	b.mark("callExec").op(wasm.OpcodeCall).u32(1)
	b.mark("lga").op(wasm.OpcodeLocalGet).u32(0)
	b.mark("lgb").op(wasm.OpcodeLocalGet).u32(0)
	b.mark("add").op(wasm.OpcodeI64Add)
	b.mark("lgc").op(wasm.OpcodeLocalGet).u32(0)
	b.mark("eqz1").op(wasm.OpcodeI64Eqz)
	b.mark("brif").op(wasm.OpcodeBrIf).u32(0)
	b.mark("eqz2").op(wasm.OpcodeI64Eqz)
	b.mark("lend").op(wasm.OpcodeEnd)
	b.mark("fend").op(wasm.OpcodeEnd)

	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{Params: []wasm.ValueType{i64t}},                                  // 0: host(i64)
			{Params: []wasm.ValueType{i64t}, Results: []wasm.ValueType{i32t}}, // 1
		},
		ImportSection: []*wasm.Import{
			{Type: wasm.ExternTypeFunc, Module: "env", Name: "log", DescFunc: 0},
			{Type: wasm.ExternTypeFunc, Module: "env", Name: "exec", DescFunc: 0},
		},
		FunctionSection: []wasm.Index{1},
		CodeSection:     []*wasm.Code{{Body: b.buf}},
	}

	g, err := AnalyzeStackTransfers(m, 2)
	TaskCheck(t, err == nil, fmt.Sprintf("analyze failed: %v", err))
	mk := b.marks

	// Loop param and result types differ.
	lp := findValue(g, ValueBlockParam, mk["loop"], 0)
	lr := findValue(g, ValueBlockResult, mk["loop"], 0)
	TaskCheck(t, lp != nil && lp.Type == i64t, "loop param missing")
	TaskCheck(t, lr != nil && lr.Type == i32t, "loop result missing")
	TaskCheck(t, lp.Type != lr.Type, "test requires differing loop param/result types")

	// The back edge references the predetermined header param identity.
	enter := findEdges(g, EdgeEnter, mk["loop"])
	TaskCheck(t, len(enter) == 1 && enter[0].Targets[0] == lp, "loop enter edge mismatch")
	TaskCheck(t, enter[0].Args[0] == findValue(g, ValueLocalGet, mk["lg0"], 0), "loop enter arg mismatch")
	brif := findEdges(g, EdgeBranchIf, mk["brif"])
	TaskCheck(t, len(brif) == 1, "want one br_if edge")
	TaskCheck(t, brif[0].TargetKind == TargetBlockParam && brif[0].TargetOffset == mk["loop"],
		"loop branch must target the header params")
	TaskCheck(t, brif[0].Targets[0] == lp, "back edge must reference the header param identity")
	TaskCheck(t, brif[0].Targets[0] != lr, "loop branch must not target the results")
	sum := findValue(g, ValueInstr, mk["add"], 0)
	TaskCheck(t, brif[0].Args[0] == sum, "br_if must carry the loop param, not the result")

	// br_if consumed the condition first; the fallthrough keeps the args.
	TaskCheck(t, findInstr(g, mk["brif"]).Inputs[0] == findValue(g, ValueInstr, mk["eqz1"], 0),
		"br_if must consume the condition first")
	TaskCheck(t, findInstr(g, mk["eqz2"]).Inputs[0] == sum, "br_if fallthrough must keep the args")

	// The loop end merges the fallthrough results; the function exit is synthetic.
	lend := findEdges(g, EdgeEnd, mk["lend"])
	TaskCheck(t, len(lend) == 1 && lend[0].Targets[0] == lr && lend[0].Args[0] == findValue(g, ValueInstr, mk["eqz2"], 0),
		"loop end merge mismatch")
	fend := findEdges(g, EdgeEnd, mk["fend"])
	fr := findValue(g, ValueFuncResult, SyntheticOffset, 0)
	TaskCheck(t, len(fend) == 1 && fend[0].TargetKind == TargetFuncResult && fend[0].TargetOffset == SyntheticOffset,
		"function exit must be synthetic")
	TaskCheck(t, fr != nil && fr.Type == i32t && fend[0].Targets[0] == fr && fend[0].Args[0] == lr, "exit merge mismatch")

	checkSorted(t, g)

	// Audit: one allowed and one forbidden import; the allowed call's arg is
	// the loop param, traceable to both the entry edge and the back edge.
	report := AuditHostCapabilities(m, []HostFuncRef{{Module: "env", Name: "log"}})
	TaskCheck(t, len(report.Calls) == 2, "audit: want two host calls")
	log, exec := report.Calls[0], report.Calls[1]
	TaskCheck(t, log.Module == "env" && log.Name == "log" && log.Allowed && log.Offset == mk["callLog"],
		"audit: allowed call mismatch")
	TaskCheck(t, exec.Module == "env" && exec.Name == "exec" && !exec.Allowed && exec.Offset == mk["callExec"],
		"audit: unlisted call must be a policy rejection")
	TaskCheck(t, len(log.Args) == 1 && sameValue(log.Args[0], lp), "audit: log arg must be the loop param")
	TaskCheck(t, len(log.Sources) == 1 && len(log.Sources[0].Origins) == 2,
		"audit: loop param must trace to both incoming edges")
	TaskCheck(t, sameValue(log.Sources[0].Origins[0], findValue(g, ValueLocalGet, mk["lg0"], 0)) &&
		sameValue(log.Sources[0].Origins[1], sum), "audit: distinct in-edge argument identities")
	TaskCheck(t, log.Sources[0].External, "audit: local.get origin must be flagged external")
	TaskCheck(t, len(exec.Sources) == 1 && exec.Sources[0].External, "audit: local.get arg is external unknown")
	TaskCheck(t, !report.Admissible(), "audit: rejected call must block admission")
}

// testConditional covers if/else arms starting from one input stack, merges
// with predecessor identity, the false edge of an if without else, and
// br_table duplicate targets that keep their selector identity.
func testConditional(t *testing.T) {
	const i32t = wasm.ValueTypeI32
	b1 := newBody()
	b1.mark("x").op(wasm.OpcodeLocalGet).u32(0)
	b1.mark("cond").op(wasm.OpcodeLocalGet).u32(0)
	b1.mark("if").op(wasm.OpcodeIf).u32(1) // (param i32) (result i32 i32)
	b1.mark("c10").op(wasm.OpcodeI32Const).i32(10)
	b1.mark("else").op(wasm.OpcodeElse)
	b1.mark("c20").op(wasm.OpcodeI32Const).i32(20)
	b1.mark("iend").op(wasm.OpcodeEnd)
	b1.mark("call").op(wasm.OpcodeCall).u32(0)
	b1.mark("fend").op(wasm.OpcodeEnd)

	b2 := newBody()
	b2.mark("v").op(wasm.OpcodeI32Const).i32(5)
	b2.mark("cond").op(wasm.OpcodeLocalGet).u32(0)
	b2.mark("if").op(wasm.OpcodeIf).u32(2) // (param i32) (result i32), no else
	b2.mark("c7").op(wasm.OpcodeI32Const).i32(7)
	b2.mark("add").op(wasm.OpcodeI32Add)
	b2.mark("iend").op(wasm.OpcodeEnd)
	b2.mark("drop").op(wasm.OpcodeDrop)
	b2.mark("blk").op(wasm.OpcodeBlock).raw(wasm.ValueTypeI32)
	b2.mark("c1").op(wasm.OpcodeI32Const).i32(1)
	b2.mark("idx").op(wasm.OpcodeLocalGet).u32(0)
	b2.mark("table").op(wasm.OpcodeBrTable).u32(2).u32(0).u32(0).u32(0) // targets 0, 0; default 0
	b2.mark("bend").op(wasm.OpcodeEnd)
	b2.mark("drop2").op(wasm.OpcodeDrop)
	b2.mark("fend").op(wasm.OpcodeEnd)

	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{Params: []wasm.ValueType{i32t, i32t}},                                  // 0: env.log(i32, i32)
			{Params: []wasm.ValueType{i32t}, Results: []wasm.ValueType{i32t, i32t}}, // 1
			{Params: []wasm.ValueType{i32t}, Results: []wasm.ValueType{i32t}},       // 2
			{Params: []wasm.ValueType{i32t}},                                        // 3
		},
		ImportSection:   []*wasm.Import{{Type: wasm.ExternTypeFunc, Module: "env", Name: "log", DescFunc: 0}},
		FunctionSection: []wasm.Index{3, 3},
		CodeSection:     []*wasm.Code{{Body: b1.buf}, {Body: b2.buf}},
	}

	// Function 1: both arms start from the same input stack and merge at end.
	g, err := AnalyzeStackTransfers(m, 1)
	TaskCheck(t, err == nil, fmt.Sprintf("analyze func 1 failed: %v", err))
	mk := b1.marks
	p0 := findValue(g, ValueBlockParam, mk["if"], 0)
	r0 := findValue(g, ValueBlockResult, mk["if"], 0)
	r1 := findValue(g, ValueBlockResult, mk["if"], 1)
	TaskCheck(t, p0 != nil && r0 != nil && r1 != nil, "if param/result nodes missing")
	enter := findEdges(g, EdgeEnter, mk["if"])
	enterElse := findEdges(g, EdgeEnterElse, mk["else"])
	x := findValue(g, ValueLocalGet, mk["x"], 0)
	TaskCheck(t, len(enter) == 1 && len(enterElse) == 1, "want enter and enter-else edges")
	TaskCheck(t, enter[0].Args[0] == x && enterElse[0].Args[0] == x, "both arms must start from the same input stack")
	TaskCheck(t, enter[0].Targets[0] == p0 && enterElse[0].Targets[0] == p0, "arm entries must share the param node")
	thenEnd := findEdges(g, EdgeEnd, mk["else"])
	elseEnd := findEdges(g, EdgeEnd, mk["iend"])
	TaskCheck(t, len(thenEnd) == 1 && len(elseEnd) == 1, "want one merge edge per arm")
	TaskCheck(t, thenEnd[0].FromOffset != elseEnd[0].FromOffset, "merge edges must carry predecessor identity")
	TaskCheck(t, thenEnd[0].Args[0] == p0 && thenEnd[0].Args[1] == findValue(g, ValueInstr, mk["c10"], 0), "then merge args mismatch")
	TaskCheck(t, elseEnd[0].Args[0] == p0 && elseEnd[0].Args[1] == findValue(g, ValueInstr, mk["c20"], 0), "else merge args mismatch")
	TaskCheck(t, thenEnd[0].Targets[0] == r0 && elseEnd[0].Targets[0] == r0, "both arms must merge into the same results")
	call := findInstr(g, mk["call"])
	TaskCheck(t, call != nil && call.Inputs[0] == r0 && call.Inputs[1] == r1, "call must consume the merge results")
	checkSorted(t, g)

	// Function 2: if without else has a valid false edge; br_table keeps
	// duplicate targets as distinct selectors.
	g2, err := AnalyzeStackTransfers(m, 2)
	TaskCheck(t, err == nil, fmt.Sprintf("analyze func 2 failed: %v", err))
	mk2 := b2.marks
	r0f := findValue(g2, ValueBlockResult, mk2["if"], 0)
	falseE := findEdges(g2, EdgeFalse, mk2["if"])
	TaskCheck(t, len(falseE) == 1, "if without else must have a false edge")
	TaskCheck(t, falseE[0].Args[0] == findValue(g2, ValueInstr, mk2["v"], 0) && falseE[0].Targets[0] == r0f,
		"false edge must carry the inputs to the results")
	thenE := findEdges(g2, EdgeEnd, mk2["iend"])
	TaskCheck(t, len(thenE) == 1 && thenE[0].Args[0] == findValue(g2, ValueInstr, mk2["add"], 0), "then merge mismatch")
	table := findEdges(g2, EdgeBranchTable, mk2["table"])
	TaskCheck(t, len(table) == 3, "duplicate br_table targets must keep every selector")
	sels := map[uint32]bool{}
	for _, e := range table {
		sels[e.Selector] = true
		TaskCheck(t, e.TargetKind == TargetBlockResult && e.TargetOffset == mk2["blk"], "br_table target mismatch")
		TaskCheck(t, len(e.Args) == 1 && e.Args[0] == findValue(g2, ValueInstr, mk2["c1"], 0), "br_table args mismatch")
	}
	TaskCheck(t, sels[0] && sels[1] && sels[2], "selector identities 0, 1 and default must survive")
	checkSorted(t, g2)

	// Audit: the merge results flow into an allowed host call; each arm's
	// contribution keeps its distinct identity.
	report := AuditHostCapabilities(m, []HostFuncRef{{Module: "env", Name: "log"}})
	TaskCheck(t, len(report.Calls) == 1, "audit: want exactly one host call")
	c := report.Calls[0]
	TaskCheck(t, c.Allowed && c.FuncIndex == 1 && c.Offset == mk["call"], "audit: call site mismatch")
	TaskCheck(t, len(c.Sources) == 2, "audit: want two arg sources")
	TaskCheck(t, len(c.Sources[0].Origins) == 1 && sameValue(c.Sources[0].Origins[0], x), "audit: first arg must trace to the shared input")
	TaskCheck(t, len(c.Sources[1].Origins) == 2 && sameValue(c.Sources[1].Origins[0], findValue(g, ValueInstr, mk["c10"], 0)) &&
		sameValue(c.Sources[1].Origins[1], findValue(g, ValueInstr, mk["c20"], 0)), "audit: second arg must trace to both arms")
	TaskCheck(t, report.Admissible(), "audit: all calls allowed, plugin must be admissible")
}

// testDeadcode covers dead code after unconditional control transfers,
// reachability recovery at an end with branch in-edges, positioned failures
// (including in dead code), normal analysis after a failure, non-function
// imports, imported-function errors, and audit incompleteness.
func testDeadcode(t *testing.T) {
	b1 := newBody()
	b1.mark("blk").op(wasm.OpcodeBlock).raw(0x40)
	b1.mark("br").op(wasm.OpcodeBr).u32(0)
	b1.mark("unr").op(wasm.OpcodeUnreachable)
	b1.mark("deadConst").op(wasm.OpcodeI32Const).i32(9)
	b1.mark("deadDrop").op(wasm.OpcodeDrop)
	b1.mark("bend").op(wasm.OpcodeEnd)
	b1.mark("live").op(wasm.OpcodeI32Const).i32(1)
	b1.mark("drop").op(wasm.OpcodeDrop)
	b1.mark("ret").op(wasm.OpcodeReturn)
	b1.mark("deadCall").op(wasm.OpcodeCall).u32(0)
	b1.mark("deadUnr").op(wasm.OpcodeUnreachable)
	b1.mark("fend").op(wasm.OpcodeEnd)

	b11 := newBody()
	b11.mark("c").op(wasm.OpcodeI32Const).i32(1)
	b11.mark("call").op(wasm.OpcodeCall).u32(0)
	b11.mark("fend").op(wasm.OpcodeEnd)

	codes := []*wasm.Code{
		{Body: b1.buf}, // 1: dead code with recovery and an unreachable call
		{Body: []byte{wasm.OpcodeI32Const, 1, wasm.OpcodeI32Const, 2, 0x6e, wasm.OpcodeDrop, wasm.OpcodeEnd}},              // 2: i32.div_u unsupported
		{Body: []byte{wasm.OpcodeUnreachable, wasm.OpcodeF32Const, 0, 0, 0xc0, 0x3f, wasm.OpcodeEnd}},                      // 3: unsupported in dead code
		{Body: []byte{wasm.OpcodeI32Const, 1, wasm.OpcodeI32Const, 2, wasm.OpcodeI64Add, wasm.OpcodeDrop, wasm.OpcodeEnd}}, // 4: type mismatch
		{Body: []byte{wasm.OpcodeBr, 5}},          // 5: bad label depth
		{Body: []byte{wasm.OpcodeI32Const, 0x80}}, // 6: truncated immediate
		{Body: []byte{wasm.OpcodeElse}},           // 7: block structure mismatch
		{Body: []byte{wasm.OpcodeLocalGet, 9}},    // 8: local index out of range
		{Body: []byte{wasm.OpcodeCall, 99}},       // 9: call index out of range
		{Body: []byte{wasm.OpcodeBlock, wasm.ValueTypeI32, wasm.OpcodeBlock, wasm.ValueTypeI64,
			wasm.OpcodeI64Const, 1, wasm.OpcodeI32Const, 0, wasm.OpcodeBrTable, 1, 0, 1,
			wasm.OpcodeEnd, wasm.OpcodeEnd, wasm.OpcodeEnd}}, // 10: br_table type sequence mismatch
		{Body: b11.buf}, // 11: reachable call to the allowed import
	}
	funcs := make([]wasm.Index, len(codes))
	for i := range funcs {
		funcs[i] = 1 // all local functions have type () -> ()
	}
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{Params: []wasm.ValueType{wasm.ValueTypeI32}}, // 0: env.log(i32)
			{}, // 1: () -> ()
		},
		ImportSection: []*wasm.Import{
			{Type: wasm.ExternTypeGlobal, Module: "m", Name: "g", DescGlobal: &wasm.GlobalType{ValType: wasm.ValueTypeI32}},
			{Type: wasm.ExternTypeFunc, Module: "env", Name: "log", DescFunc: 0},
		},
		FunctionSection: funcs,
		CodeSection:     codes,
	}
	mk := b1.marks

	// The main function analyzes cleanly: dead code produces no nodes, no
	// instruction records and no reachable edges, and the end recovers.
	g, err := AnalyzeStackTransfers(m, 1)
	TaskCheck(t, err == nil, fmt.Sprintf("analyze failed: %v", err))
	TaskCheck(t, findValue(g, ValueInstr, mk["deadConst"], 0) == nil, "dead code must not produce value nodes")
	TaskCheck(t, findValue(g, ValueInstr, mk["live"], 0) != nil, "end must recover reachability from branch in-edges")
	for _, off := range []uint32{mk["unr"], mk["deadConst"], mk["deadDrop"], mk["deadCall"], mk["deadUnr"]} {
		TaskCheck(t, findInstr(g, off) == nil, fmt.Sprintf("dead instruction at %d must have no record", off))
	}
	TaskCheck(t, findInstr(g, mk["live"]) != nil, "recovered instruction must have a record")
	br := findEdges(g, EdgeBranch, mk["br"])
	TaskCheck(t, len(br) == 1 && br[0].TargetKind == TargetBlockResult && br[0].TargetOffset == mk["blk"], "br edge mismatch")
	TaskCheck(t, len(findEdges(g, EdgeEnd, mk["bend"])) == 0, "dead fallthrough must not emit an end edge")
	ret := findEdges(g, EdgeReturn, mk["ret"])
	TaskCheck(t, len(ret) == 1 && ret[0].TargetKind == TargetFuncResult && ret[0].TargetOffset == SyntheticOffset,
		"return must target the synthetic function exit")
	checkSorted(t, g)

	// Imported functions and out-of-range indices are clear errors.
	_, err = AnalyzeStackTransfers(m, 0)
	checkErr(t, err, ErrImportFunction, 0)
	_, err = AnalyzeStackTransfers(m, 100)
	checkErr(t, err, ErrFunctionIndex, 100)

	// Positioned failures, in dead code too; each carries the function index.
	_, err = AnalyzeStackTransfers(m, 2)
	checkErr(t, err, ErrUnsupportedOpcode, 2)
	_, err = AnalyzeStackTransfers(m, 3)
	checkErr(t, err, ErrUnsupportedOpcode, 3)
	_, err = AnalyzeStackTransfers(m, 4)
	checkErr(t, err, ErrStackType, 4)
	_, err = AnalyzeStackTransfers(m, 5)
	checkErr(t, err, ErrLabelDepth, 5)
	_, err = AnalyzeStackTransfers(m, 6)
	checkErr(t, err, ErrImmediate, 6)
	_, err = AnalyzeStackTransfers(m, 7)
	checkErr(t, err, ErrStructure, 7)
	_, err = AnalyzeStackTransfers(m, 8)
	checkErr(t, err, ErrIndex, 8)
	_, err = AnalyzeStackTransfers(m, 9)
	checkErr(t, err, ErrIndex, 9)
	_, err = AnalyzeStackTransfers(m, 10)
	checkErr(t, err, ErrStackType, 10)

	// Normal analysis still works after a failure.
	_, err = AnalyzeStackTransfers(m, 2)
	TaskCheck(t, err != nil, "func 2 must keep failing")
	g2, err := AnalyzeStackTransfers(m, 1)
	TaskCheck(t, err == nil && g2 != nil && len(g2.Values) == len(g.Values), "analysis after failure must be unaffected")

	// Audit: the unreachable call is absent, the unsupported function is
	// incomplete, and only the reachable allowlisted call is reported.
	report := AuditHostCapabilities(m, []HostFuncRef{{Module: "env", Name: "log"}})
	TaskCheck(t, len(report.Functions) == len(codes), "audit: want one entry per local function")
	TaskCheck(t, report.Functions[0].Complete, "audit: main function must be complete")
	TaskCheck(t, !report.Functions[1].Complete, "audit: unsupported function must be incomplete")
	e, ok := report.Functions[1].Err.(*Error)
	TaskCheck(t, ok && e.Kind == ErrUnsupportedOpcode, "audit: incomplete function must report the unsupported opcode")
	TaskCheck(t, len(report.Calls) == 1, "audit: unreachable calls must not be reported")
	c := report.Calls[0]
	TaskCheck(t, c.FuncIndex == 11 && c.Offset == b11.marks["call"], "audit: wrong reachable call site")
	TaskCheck(t, c.Module == "env" && c.Name == "log" && c.CallIndex == 0,
		"audit: non-function imports must not shift the function namespace")
	TaskCheck(t, c.Allowed, "audit: allowlisted call must be allowed")
	TaskCheck(t, !report.Admissible(), "audit: incomplete analysis must block admission")
}
