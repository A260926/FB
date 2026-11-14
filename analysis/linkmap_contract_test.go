package analysis_test

import (
	"fmt"
	"reflect"
	"strings"
	"testing"

	"github.com/tetratelabs/wabin/analysis"
	"github.com/tetratelabs/wabin/binary"
	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/task_support/taskcheck"
	"github.com/tetratelabs/wabin/wasm"
)

// --- helpers ---------------------------------------------------------------

func i32const(v int32) *wasm.ConstantExpression {
	return &wasm.ConstantExpression{Opcode: wasm.OpcodeI32Const, Data: leb128.EncodeInt32(v)}
}

func i64const(v int64) *wasm.ConstantExpression {
	return &wasm.ConstantExpression{Opcode: wasm.OpcodeI64Const, Data: leb128.EncodeInt64(v)}
}

func globalGet(idx uint32) *wasm.ConstantExpression {
	return &wasm.ConstantExpression{Opcode: wasm.OpcodeGlobalGet, Data: leb128.EncodeUint32(idx)}
}

func u32ptr(v uint32) *uint32 { return &v }

func input(name string, m *wasm.Module) analysis.ModuleInput {
	return analysis.ModuleInput{Name: name, Binary: binary.EncodeModule(m)}
}

// rawInput encodes m and appends extra raw sections. The repo encoder only supports the legacy active element
// form, so non-legacy element segments are appended by hand; the decoder does not require section order.
func rawInput(name string, m *wasm.Module, extraSections ...[]byte) analysis.ModuleInput {
	bin := binary.EncodeModule(m)
	for _, s := range extraSections {
		bin = append(bin, s...)
	}
	return analysis.ModuleInput{Name: name, Binary: bin}
}

func section(id byte, payload []byte) []byte {
	out := []byte{id}
	out = append(out, leb128.EncodeUint32(uint32(len(payload)))...)
	return append(out, payload...)
}

func elementSection(segs ...[]byte) []byte {
	payload := leb128.EncodeUint32(uint32(len(segs)))
	for _, s := range segs {
		payload = append(payload, s...)
	}
	return section(wasm.SectionIDElement, payload)
}

// importSection builds a raw import section. The repo encoder hardcodes funcref for table imports, so
// non-funcref table imports are hand-encoded.
func importSection(imps ...[]byte) []byte {
	payload := leb128.EncodeUint32(uint32(len(imps)))
	for _, i := range imps {
		payload = append(payload, i...)
	}
	return section(wasm.SectionIDImport, payload)
}

func rawImport(module, name string, kind wasm.ExternType, desc []byte) []byte {
	out := leb128.EncodeUint32(uint32(len(module)))
	out = append(out, module...)
	out = append(out, leb128.EncodeUint32(uint32(len(name)))...)
	out = append(out, name...)
	out = append(out, kind)
	return append(out, desc...)
}

func constExprBytes(expr *wasm.ConstantExpression) []byte {
	out := []byte{expr.Opcode}
	out = append(out, expr.Data...)
	return append(out, wasm.OpcodeEnd)
}

// legacyElemSegment is an active element segment on table 0 (prefix 0) with raw function indices.
func legacyElemSegment(offset int32, funcs ...uint32) []byte {
	out := []byte{0x00}
	out = append(out, constExprBytes(i32const(offset))...)
	out = append(out, leb128.EncodeUint32(uint32(len(funcs)))...)
	for _, f := range funcs {
		out = append(out, leb128.EncodeUint32(f)...)
	}
	return out
}

// tableIndexedElemSegment is an active element segment with an explicit table index (prefix 2).
func tableIndexedElemSegment(table uint32, offset int32, funcs ...uint32) []byte {
	out := []byte{0x02}
	out = append(out, leb128.EncodeUint32(table)...)
	out = append(out, constExprBytes(i32const(offset))...)
	out = append(out, 0x00) // elemkind: funcref
	out = append(out, leb128.EncodeUint32(uint32(len(funcs)))...)
	for _, f := range funcs {
		out = append(out, leb128.EncodeUint32(f)...)
	}
	return out
}

// constExprElemSegment is an active funcref element segment whose items are constant expressions (prefix 4),
// which is the only form that can write null.
func constExprElemSegment(offset int32, items ...[]byte) []byte {
	out := []byte{0x04}
	out = append(out, constExprBytes(i32const(offset))...)
	out = append(out, leb128.EncodeUint32(uint32(len(items)))...)
	for _, it := range items {
		out = append(out, it...)
	}
	return out
}

func refNullFuncref() []byte { return []byte{wasm.OpcodeRefNull, wasm.RefTypeFuncref, wasm.OpcodeEnd} }

func refFunc(idx uint32) []byte {
	out := []byte{wasm.OpcodeRefFunc}
	out = append(out, leb128.EncodeUint32(idx)...)
	return append(out, wasm.OpcodeEnd)
}

// passiveElemSegment is a passive funcref element segment (prefix 1).
func passiveElemSegment(funcs ...uint32) []byte {
	out := []byte{0x01, 0x00}
	out = append(out, leb128.EncodeUint32(uint32(len(funcs)))...)
	for _, f := range funcs {
		out = append(out, leb128.EncodeUint32(f)...)
	}
	return out
}

// declarativeElemSegment is a declarative funcref element segment (prefix 3).
func declarativeElemSegment(funcs ...uint32) []byte {
	out := []byte{0x03, 0x00}
	out = append(out, leb128.EncodeUint32(uint32(len(funcs)))...)
	for _, f := range funcs {
		out = append(out, leb128.EncodeUint32(f)...)
	}
	return out
}

func buildErr(inputs []analysis.ModuleInput, order []string) error {
	_, err := analysis.BuildBundle(inputs, order)
	return err
}

func hasErr(err error, substr string) bool {
	return err != nil && strings.Contains(err.Error(), substr)
}

func mustBundle(c *taskcheck.C, inputs []analysis.ModuleInput, order []string) (*analysis.Bundle, bool) {
	b, err := analysis.BuildBundle(inputs, order)
	c.Check(err == nil, "BuildBundle: %v", err)
	return b, err == nil
}

func memLayoutSummary(frags []analysis.MemoryFragment) []string {
	var out []string
	for _, f := range frags {
		if f.Written {
			out = append(out, fmt.Sprintf("[%d,%d)%s#%d+%d:%q", f.Start, f.End, f.Module, f.Segment, f.Offset, f.Data))
		} else {
			out = append(out, fmt.Sprintf("[%d,%d)zero", f.Start, f.End))
		}
	}
	return out
}

func tabLayoutSummary(frags []analysis.TableFragment) []string {
	var out []string
	for _, f := range frags {
		if !f.Written {
			out = append(out, fmt.Sprintf("[%d,%d)unwritten", f.Start, f.End))
			continue
		}
		var slots []string
		for _, s := range f.Slots {
			if s.Null {
				slots = append(slots, "null")
			} else {
				slots = append(slots, s.Function.String())
			}
		}
		out = append(out, fmt.Sprintf("[%d,%d)%s#%d+%d:%s", f.Start, f.End, f.Module, f.Segment, f.Offset, strings.Join(slots, ",")))
	}
	return out
}

func writeSummary(ws []analysis.WriteRecord) []string {
	var out []string
	for _, w := range ws {
		out = append(out, fmt.Sprintf("%s %s#%d [%d,%d) chain=%d", w.Resource, w.Writer, w.Segment, w.Start, w.End, len(w.Chain)))
	}
	return out
}

func overrideSummary(os []analysis.OverrideRecord) []string {
	var out []string
	for _, o := range os {
		out = append(out, fmt.Sprintf("%s %s#%d [%d,%d) prev=%s#%d",
			o.Resource, o.Writer, o.Segment, o.Start, o.End, o.Previous.Module, o.Previous.Segment))
	}
	return out
}

// --- TestAliasResolution ----------------------------------------------------

func TestAliasResolution(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		i32 := wasm.ValueTypeI32
		core := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0, 0},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}, {Body: []byte{wasm.OpcodeEnd}}},
			MemorySection:   &wasm.Memory{Min: 2, Max: 4, IsMaxEncoded: true},
			TableSection:    []*wasm.Table{{Min: 4, Max: u32ptr(8), Type: wasm.RefTypeFuncref}},
			GlobalSection:   []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: i32const(64)}},
			ExportSection: []*wasm.Export{
				{Name: "f", Type: wasm.ExternTypeFunc, Index: 0},
				{Name: "f2", Type: wasm.ExternTypeFunc, Index: 1},
				{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
				{Name: "tab", Type: wasm.ExternTypeTable, Index: 0},
				{Name: "g", Type: wasm.ExternTypeGlobal, Index: 0},
			},
		}
		// mid interleaves imports of every kind: the import section position is not the kind index.
		mid := &wasm.Module{
			TypeSection: []*wasm.FunctionType{{}},
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeFunc, Module: "core", Name: "f", DescFunc: 0},
				{Type: wasm.ExternTypeMemory, Module: "core", Name: "mem", DescMem: &wasm.Memory{Min: 1, Max: 4, IsMaxEncoded: true}},
				{Type: wasm.ExternTypeFunc, Module: "core", Name: "f2", DescFunc: 0},
				{Type: wasm.ExternTypeGlobal, Module: "core", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i32}},
				{Type: wasm.ExternTypeTable, Module: "core", Name: "tab", DescTable: &wasm.Table{Min: 2, Max: u32ptr(8), Type: wasm.RefTypeFuncref}},
			},
			ExportSection: []*wasm.Export{
				{Name: "f", Type: wasm.ExternTypeFunc, Index: 0},
				{Name: "f2", Type: wasm.ExternTypeFunc, Index: 1},
				{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
				{Name: "tab", Type: wasm.ExternTypeTable, Index: 0},
				{Name: "g", Type: wasm.ExternTypeGlobal, Index: 0},
			},
		}
		app := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "mid", Name: "mem", DescMem: &wasm.Memory{Min: 2, Max: 4, IsMaxEncoded: true}},
				{Type: wasm.ExternTypeGlobal, Module: "mid", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i32}},
			},
			DataSection: []*wasm.DataSegment{
				{OffsetExpression: globalGet(0), Init: []byte("APP")},
			},
		}
		// other exports a memory under the same name "mem": it must stay a distinct resource.
		other := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory}},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: i32const(0), Init: []byte("OT")}},
		}
		inputs := []analysis.ModuleInput{input("core", core), input("mid", mid), input("app", app), input("other", other)}
		b, ok := mustBundle(c, inputs, []string{"core", "mid", "app", "other"})
		if !ok {
			return
		}

		// Two-hop alias: app -> mid -> core, with every hop recorded.
		res, err := b.Resolve("app", wasm.ExternTypeMemory, 0)
		c.Check(err == nil, "resolve app memory: %v", err)
		c.Check(res.Final == (analysis.SymbolRef{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0}), "final: %s", res.Final)
		c.Check(reflect.DeepEqual(res.Chain, []analysis.SymbolRef{
			{Module: "app", Kind: wasm.ExternTypeMemory, Index: 0},
			{Module: "mid", Kind: wasm.ExternTypeMemory, Index: 0},
			{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0},
		}), "chain: %v", res.Chain)

		// Interleaved imports: mid's "f2" sits at import section position 2, but func index 1.
		res, err = b.Resolve("mid", wasm.ExternTypeFunc, 1)
		c.Check(err == nil, "resolve mid f2: %v", err)
		c.Check(res.Final == (analysis.SymbolRef{Module: "core", Kind: wasm.ExternTypeFunc, Index: 1}), "final: %s", res.Final)
		c.Check(reflect.DeepEqual(res.Chain, []analysis.SymbolRef{
			{Module: "mid", Kind: wasm.ExternTypeFunc, Index: 1},
			{Module: "core", Kind: wasm.ExternTypeFunc, Index: 1},
		}), "chain: %v", res.Chain)

		// Table and global aliases.
		res, err = b.Resolve("mid", wasm.ExternTypeTable, 0)
		c.Check(err == nil && res.Final == (analysis.SymbolRef{Module: "core", Kind: wasm.ExternTypeTable, Index: 0}), "table: %v %s", err, res.Final)
		res, err = b.Resolve("app", wasm.ExternTypeGlobal, 0)
		c.Check(err == nil, "resolve app global: %v", err)
		c.Check(res.Final == (analysis.SymbolRef{Module: "core", Kind: wasm.ExternTypeGlobal, Index: 0}), "final: %s", res.Final)
		c.Check(len(res.Chain) == 3, "two-hop global chain: %v", res.Chain)

		// A definition resolves to itself with a single-hop chain.
		res, err = b.Resolve("core", wasm.ExternTypeMemory, 0)
		c.Check(err == nil && len(res.Chain) == 1 && res.Final == res.Chain[0], "self: %v %v", err, res.Chain)

		// Same export name "mem" in another module is not merged with core's memory.
		res, err = b.Resolve("other", wasm.ExternTypeMemory, 0)
		c.Check(err == nil && res.Final == (analysis.SymbolRef{Module: "other", Kind: wasm.ExternTypeMemory, Index: 0}), "other: %v %s", err, res.Final)
		c.Check(reflect.DeepEqual(b.Resources(wasm.ExternTypeMemory), []analysis.ResourceID{
			{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0},
			{Module: "other", Kind: wasm.ExternTypeMemory, Index: 0},
		}), "memories: %v", b.Resources(wasm.ExternTypeMemory))

		// The same layout is visible through every alias of the shared memory.
		wantLayout := []string{"[0,64)zero", "[64,67)app#0+0:\"APP\"", "[67,128)zero"}
		for _, m := range []string{"core", "mid", "app"} {
			frags, err := b.MemoryLayout(m, 0, 0, 128)
			c.Check(err == nil, "layout via %s: %v", m, err)
			c.Check(reflect.DeepEqual(memLayoutSummary(frags), wantLayout), "layout via %s: %v", m, memLayoutSummary(frags))
		}
		frags, err := b.MemoryLayout("other", 0, 0, 4)
		c.Check(err == nil, "other layout: %v", err)
		c.Check(reflect.DeepEqual(memLayoutSummary(frags), []string{"[0,2)other#0+0:\"OT\"", "[2,4)zero"}), "other layout: %v", memLayoutSummary(frags))

		// Audit: app writes core's memory through a two-hop alias and is not permitted to.
		rep := b.Audit(analysis.AuditPolicy{Allow: map[analysis.ResourceID][]string{
			{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0}:  {"core"},
			{Module: "other", Kind: wasm.ExternTypeMemory, Index: 0}: {"other"},
		}})
		c.Check(reflect.DeepEqual(writeSummary(rep.Unauthorized), []string{
			"core.memory[0] app#0 [64,67) chain=3",
		}), "unauthorized: %v", writeSummary(rep.Unauthorized))
		c.Check(len(rep.Overrides) == 0, "overrides: %v", overrideSummary(rep.Overrides))

		// Duplicate module names are rejected.
		err = buildErr([]analysis.ModuleInput{input("core", core), input("core", core)}, []string{"core", "core"})
		c.Check(hasErr(err, "duplicate module name"), "duplicate: %v", err)

		// initOrder must list every module exactly once.
		err = buildErr(inputs, []string{"core"})
		c.Check(hasErr(err, "initOrder"), "short initOrder: %v", err)
		err = buildErr(inputs, []string{"core", "mid", "app", "ghost"})
		c.Check(hasErr(err, "unknown module"), "unknown initOrder: %v", err)
		err = buildErr(inputs, []string{"core", "mid", "app", "app"})
		c.Check(hasErr(err, "listed twice"), "dup initOrder: %v", err)

		// Import from a missing module.
		imp := &wasm.Module{
			TypeSection:   []*wasm.FunctionType{{}},
			ImportSection: []*wasm.Import{{Type: wasm.ExternTypeFunc, Module: "ghost", Name: "x", DescFunc: 0}},
		}
		err = buildErr([]analysis.ModuleInput{input("imp", imp)}, []string{"imp"})
		c.Check(hasErr(err, `"ghost"`) && hasErr(err, "not found"), "missing module: %v", err)

		// Import of a missing export.
		imp.ImportSection = []*wasm.Import{{Type: wasm.ExternTypeFunc, Module: "core", Name: "nope", DescFunc: 0}}
		err = buildErr([]analysis.ModuleInput{input("core", core), input("imp", imp)}, []string{"core", "imp"})
		c.Check(hasErr(err, "no export"), "missing export: %v", err)

		// Import with the wrong kind.
		imp.ImportSection = []*wasm.Import{{Type: wasm.ExternTypeGlobal, Module: "core", Name: "mem", DescGlobal: &wasm.GlobalType{ValType: i32}}}
		err = buildErr([]analysis.ModuleInput{input("core", core), input("imp", imp)}, []string{"core", "imp"})
		c.Check(hasErr(err, "kind mismatch"), "kind mismatch: %v", err)

		// A pure alias cycle has no definition to land on.
		x := &wasm.Module{
			ImportSection: []*wasm.Import{{Type: wasm.ExternTypeMemory, Module: "y", Name: "m", DescMem: &wasm.Memory{Min: 1}}},
			ExportSection: []*wasm.Export{{Name: "m", Type: wasm.ExternTypeMemory}},
		}
		y := &wasm.Module{
			ImportSection: []*wasm.Import{{Type: wasm.ExternTypeMemory, Module: "x", Name: "m", DescMem: &wasm.Memory{Min: 1}}},
			ExportSection: []*wasm.Export{{Name: "m", Type: wasm.ExternTypeMemory}},
		}
		err = buildErr([]analysis.ModuleInput{input("x", x), input("y", y)}, []string{"x", "y"})
		c.Check(hasErr(err, "cycle") && hasErr(err, "x.memory[0]") && hasErr(err, "y.memory[0]"), "alias cycle: %v", err)

		// Cycles between modules are fine when every symbol lands on an actual definition.
		modA := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			ImportSection:   []*wasm.Import{{Type: wasm.ExternTypeFunc, Module: "modB", Name: "g", DescFunc: 0}},
			FunctionSection: []wasm.Index{0},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
			ExportSection:   []*wasm.Export{{Name: "f", Type: wasm.ExternTypeFunc, Index: 1}},
		}
		modB := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			ImportSection:   []*wasm.Import{{Type: wasm.ExternTypeFunc, Module: "modA", Name: "f", DescFunc: 0}},
			FunctionSection: []wasm.Index{0},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
			ExportSection:   []*wasm.Export{{Name: "g", Type: wasm.ExternTypeFunc, Index: 1}},
		}
		b2, err := analysis.BuildBundle([]analysis.ModuleInput{input("modA", modA), input("modB", modB)}, []string{"modA", "modB"})
		c.Check(err == nil, "mutual imports: %v", err)
		if err == nil {
			res, err = b2.Resolve("modA", wasm.ExternTypeFunc, 0)
			c.Check(err == nil && res.Final == (analysis.SymbolRef{Module: "modB", Kind: wasm.ExternTypeFunc, Index: 1}), "modA func0: %v %s", err, res.Final)
			res, err = b2.Resolve("modB", wasm.ExternTypeFunc, 0)
			c.Check(err == nil && res.Final == (analysis.SymbolRef{Module: "modA", Kind: wasm.ExternTypeFunc, Index: 1}), "modB func0: %v %s", err, res.Final)
		}

		// An export index out of range is a bad index even when nothing imports it.
		badexp := &wasm.Module{ExportSection: []*wasm.Export{{Name: "f", Type: wasm.ExternTypeFunc, Index: 3}}}
		err = buildErr([]analysis.ModuleInput{input("badexp", badexp)}, []string{"badexp"})
		c.Check(hasErr(err, "out of range") && hasErr(err, `"badexp"`), "bad export: %v", err)
	})
}

// --- TestTypeCompatibility --------------------------------------------------

func TestTypeCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		i32, i64 := wasm.ValueTypeI32, wasm.ValueTypeI64
		real := &wasm.Module{
			TypeSection: []*wasm.FunctionType{
				{Params: []wasm.ValueType{i32, i32}, Results: []wasm.ValueType{i32}},
				{},
			},
			FunctionSection: []wasm.Index{0},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
			MemorySection:   &wasm.Memory{Min: 2, Max: 2, IsMaxEncoded: true},
			TableSection:    []*wasm.Table{{Min: 4, Max: u32ptr(4), Type: wasm.RefTypeFuncref}},
			GlobalSection:   []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: i32const(7)}},
			ExportSection: []*wasm.Export{
				{Name: "f", Type: wasm.ExternTypeFunc, Index: 0},
				{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
				{Name: "tab", Type: wasm.ExternTypeTable, Index: 0},
				{Name: "g", Type: wasm.ExternTypeGlobal, Index: 0},
			},
		}
		// mid declares wider (looser) limits than the real resource: min 1 page and no max.
		mid := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "real", Name: "mem", DescMem: &wasm.Memory{Min: 1}},
				{Type: wasm.ExternTypeTable, Module: "real", Name: "tab", DescTable: &wasm.Table{Min: 2, Type: wasm.RefTypeFuncref}},
			},
			ExportSection: []*wasm.Export{
				{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
				{Name: "tab", Type: wasm.ExternTypeTable, Index: 0},
			},
		}
		// user declares the same func signature at a different type index, and limits tighter than mid's
		// declaration: both must be checked against the final definition, not the intermediate alias.
		user := &wasm.Module{
			TypeSection: []*wasm.FunctionType{
				{Params: []wasm.ValueType{i32, i32}, Results: []wasm.ValueType{i32}},
			},
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeFunc, Module: "real", Name: "f", DescFunc: 0},
				{Type: wasm.ExternTypeMemory, Module: "mid", Name: "mem", DescMem: &wasm.Memory{Min: 2, Max: 2, IsMaxEncoded: true}},
				{Type: wasm.ExternTypeTable, Module: "mid", Name: "tab", DescTable: &wasm.Table{Min: 4, Max: u32ptr(4), Type: wasm.RefTypeFuncref}},
				{Type: wasm.ExternTypeGlobal, Module: "real", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i32}},
			},
			// One page (65536) beyond mid's declared min, still inside real's 2 pages.
			DataSection: []*wasm.DataSegment{{OffsetExpression: i32const(70000), Init: []byte("XY")}},
		}
		b, ok := mustBundle(c, []analysis.ModuleInput{input("real", real), input("mid", mid), input("user", user)},
			[]string{"real", "mid", "user"})
		if !ok {
			return
		}

		// The effective capacity comes from the final definition's initial Min (2 pages), not from the
		// intermediate declaration (1 page).
		cap, err := b.MemoryCapacity("user", 0)
		c.Check(err == nil && cap == 2*65536, "capacity: %v %d", err, cap)
		frags, err := b.MemoryLayout("user", 0, 69998, 70004)
		c.Check(err == nil, "layout: %v", err)
		c.Check(reflect.DeepEqual(memLayoutSummary(frags), []string{
			"[69998,70000)zero", "[70000,70002)user#0+0:\"XY\"", "[70002,70004)zero",
		}), "layout: %v", memLayoutSummary(frags))

		// Same signature at a different type index links fine.
		res, err := b.Resolve("user", wasm.ExternTypeFunc, 0)
		c.Check(err == nil && res.Final == (analysis.SymbolRef{Module: "real", Kind: wasm.ExternTypeFunc, Index: 0}), "func: %v %s", err, res.Final)
		tcap, err := b.TableCapacity("user", 0)
		c.Check(err == nil && tcap == 4, "table capacity: %v %d", err, tcap)

		// consumer builds a module importing imp, with two type entries: (i32,i32)->i32 at 0 and (i64)->() at 1.
		consumer := func(imps ...*wasm.Import) *wasm.Module {
			return &wasm.Module{
				TypeSection: []*wasm.FunctionType{
					{Params: []wasm.ValueType{i32, i32}, Results: []wasm.ValueType{i32}},
					{Params: []wasm.ValueType{i64}},
				},
				ImportSection: imps,
			}
		}
		tryImport := func(imp *wasm.Import) error {
			return buildErr([]analysis.ModuleInput{input("real", real), input("c", consumer(imp))}, []string{"real", "c"})
		}

		// Function imports compare parameter and result structure.
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeFunc, Module: "real", Name: "f", DescFunc: 1})
		c.Check(hasErr(err, "does not match") && hasErr(err, `"c"`), "func signature: %v", err)
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeFunc, Module: "real", Name: "f", DescFunc: 9})
		c.Check(hasErr(err, "type index 9 out of range"), "func type index: %v", err)

		// Global imports compare value type and mutability.
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeGlobal, Module: "real", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i32, Mutable: true}})
		c.Check(hasErr(err, "does not match") && hasErr(err, "mutable"), "global mutability: %v", err)
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeGlobal, Module: "real", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i64}})
		c.Check(hasErr(err, "does not match") && hasErr(err, "i64"), "global value type: %v", err)

		// Memory limits: actual Min must cover the request, and a requested Max must exist and not be exceeded.
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeMemory, Module: "real", Name: "mem", DescMem: &wasm.Memory{Min: 3}})
		c.Check(hasErr(err, "below required min"), "memory min: %v", err)
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeMemory, Module: "real", Name: "mem", DescMem: &wasm.Memory{Min: 1, Max: 1, IsMaxEncoded: true}})
		c.Check(hasErr(err, "exceeds required max"), "memory max: %v", err)

		// A requested max cannot be satisfied by a definition without one.
		real2 := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory}},
		}
		err = buildErr([]analysis.ModuleInput{
			input("real2", real2),
			input("c", consumer(&wasm.Import{Type: wasm.ExternTypeMemory, Module: "real2", Name: "mem", DescMem: &wasm.Memory{Min: 1, Max: 1, IsMaxEncoded: true}})),
		}, []string{"real2", "c"})
		c.Check(hasErr(err, "no max"), "memory missing max: %v", err)

		// Table imports also compare the reference type. The repo encoder hardcodes funcref for table imports,
		// so the externref import is hand-encoded.
		err = buildErr([]analysis.ModuleInput{
			input("real", real),
			rawInput("c", &wasm.Module{}, importSection(rawImport("real", "tab", wasm.ExternTypeTable,
				[]byte{wasm.RefTypeExternref, 0x00, 0x01}))), // externref table, limits min 1
		}, []string{"real", "c"})
		c.Check(hasErr(err, "table type"), "table reftype: %v", err)
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeTable, Module: "real", Name: "tab", DescTable: &wasm.Table{Min: 9, Type: wasm.RefTypeFuncref}})
		c.Check(hasErr(err, "below required min"), "table min: %v", err)
		err = tryImport(&wasm.Import{Type: wasm.ExternTypeTable, Module: "real", Name: "tab", DescTable: &wasm.Table{Min: 1, Max: u32ptr(2), Type: wasm.RefTypeFuncref}})
		c.Check(hasErr(err, "exceeds required max"), "table max: %v", err)

		// An intermediate alias declaring wider limits than the real resource is itself invalid.
		mid2 := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "real", Name: "mem", DescMem: &wasm.Memory{Min: 3}},
			},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory}},
		}
		err = buildErr([]analysis.ModuleInput{input("real", real), input("mid2", mid2)}, []string{"real", "mid2"})
		c.Check(hasErr(err, "below required min") && hasErr(err, `"mid2"`), "wider intermediate: %v", err)
	})
}

// --- TestGlobalOffsets ------------------------------------------------------

func TestGlobalOffsets(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		i32, i64 := wasm.ValueTypeI32, wasm.ValueTypeI64
		base := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1, Max: 1, IsMaxEncoded: true},
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: i32const(48)}},
			ExportSection: []*wasm.Export{
				{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
				{Name: "g0", Type: wasm.ExternTypeGlobal, Index: 0},
			},
		}
		// mid's own global is initialized from its import of base's global.
		mid := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeGlobal, Module: "base", Name: "g0", DescGlobal: &wasm.GlobalType{ValType: i32}},
			},
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: globalGet(0)}},
			ExportSection: []*wasm.Export{{Name: "g1", Type: wasm.ExternTypeGlobal, Index: 1}},
		}
		app := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "base", Name: "mem", DescMem: &wasm.Memory{Min: 1, Max: 1, IsMaxEncoded: true}},
				{Type: wasm.ExternTypeGlobal, Module: "mid", Name: "g1", DescGlobal: &wasm.GlobalType{ValType: i32}},
			},
			DataSection: []*wasm.DataSegment{
				{OffsetExpression: globalGet(0), Init: []byte("GA")}, // resolves to 48 across two modules
				{OffsetExpression: i32const(0), Init: []byte("HEAD")},
			},
		}
		b, ok := mustBundle(c, []analysis.ModuleInput{input("base", base), input("mid", mid), input("app", app)},
			[]string{"base", "mid", "app"})
		if !ok {
			return
		}
		frags, err := b.MemoryLayout("base", 0, 0, 52)
		c.Check(err == nil, "layout: %v", err)
		c.Check(reflect.DeepEqual(memLayoutSummary(frags), []string{
			"[0,4)app#1+0:\"HEAD\"", "[4,48)zero", "[48,50)app#0+0:\"GA\"", "[50,52)zero",
		}), "layout: %v", memLayoutSummary(frags))

		// A negative i32.const is reinterpreted as a huge uint32; the segment end check uses wide integers and
		// must not wrap around.
		neg := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: i32const(-1), Init: []byte("X")}},
		}
		err = buildErr([]analysis.ModuleInput{input("neg", neg)}, []string{"neg"})
		c.Check(hasErr(err, "exceeds capacity") && hasErr(err, `"neg"`), "negative offset: %v", err)

		// Offset expressions other than i32.const and global.get are unsupported.
		bad := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: i64const(0), Init: []byte("X")}},
		}
		err = buildErr([]analysis.ModuleInput{input("bad", bad)}, []string{"bad"})
		c.Check(hasErr(err, "unsupported offset expression") && hasErr(err, "data segment 0"), "unsupported: %v", err)

		// A mutable global cannot be used as an offset, even when the import itself is valid.
		gmut := &wasm.Module{
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32, Mutable: true}, Init: i32const(0)}},
			ExportSection: []*wasm.Export{{Name: "g", Type: wasm.ExternTypeGlobal}},
		}
		user := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeGlobal, Module: "gmut", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i32, Mutable: true}},
			},
			DataSection: []*wasm.DataSegment{{OffsetExpression: globalGet(0), Init: []byte("X")}},
		}
		err = buildErr([]analysis.ModuleInput{input("gmut", gmut), input("user", user)}, []string{"gmut", "user"})
		c.Check(hasErr(err, "not an immutable i32"), "mutable global: %v", err)

		// A non-i32 global cannot be used as an offset either.
		gi64 := &wasm.Module{
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i64}, Init: i64const(0)}},
			ExportSection: []*wasm.Export{{Name: "g", Type: wasm.ExternTypeGlobal}},
		}
		user.ImportSection = []*wasm.Import{
			{Type: wasm.ExternTypeGlobal, Module: "gi64", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i64}},
		}
		err = buildErr([]analysis.ModuleInput{input("gi64", gi64), input("user", user)}, []string{"gi64", "user"})
		c.Check(hasErr(err, "not an immutable i32"), "i64 global: %v", err)

		// The offset global must be imported, not defined in the same module.
		selfg := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: i32const(0)}},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: globalGet(0), Init: []byte("X")}},
		}
		err = buildErr([]analysis.ModuleInput{input("selfg", selfg)}, []string{"selfg"})
		c.Check(hasErr(err, "not an imported global"), "defined global: %v", err)

		// A global.get index out of range is a bad index.
		nog := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: globalGet(7), Init: []byte("X")}},
		}
		err = buildErr([]analysis.ModuleInput{input("nog", nog)}, []string{"nog"})
		c.Check(hasErr(err, "out of range"), "global index: %v", err)

		// A dependency cycle between global initializers is reported on its own.
		memmod := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory}},
		}
		ga := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "memmod", Name: "mem", DescMem: &wasm.Memory{Min: 1}},
				{Type: wasm.ExternTypeGlobal, Module: "gb", Name: "h", DescGlobal: &wasm.GlobalType{ValType: i32}},
			},
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: globalGet(0)}},
			ExportSection: []*wasm.Export{{Name: "g", Type: wasm.ExternTypeGlobal, Index: 1}},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: globalGet(0), Init: []byte("X")}},
		}
		gb := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeGlobal, Module: "ga", Name: "g", DescGlobal: &wasm.GlobalType{ValType: i32}},
			},
			GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: globalGet(0)}},
			ExportSection: []*wasm.Export{{Name: "h", Type: wasm.ExternTypeGlobal, Index: 1}},
		}
		err = buildErr([]analysis.ModuleInput{input("memmod", memmod), input("ga", ga), input("gb", gb)},
			[]string{"memmod", "ga", "gb"})
		c.Check(hasErr(err, "dependency cycle") && hasErr(err, "ga") && hasErr(err, "gb"), "global cycle: %v", err)
	})
}

// --- TestResourceImage ------------------------------------------------------

func TestResourceImage(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		cf := wasm.Index(0)
		core := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
			MemorySection:   &wasm.Memory{Min: 2, Max: 4, IsMaxEncoded: true},
			TableSection: []*wasm.Table{
				{Min: 4, Type: wasm.RefTypeFuncref},
				{Min: 3, Type: wasm.RefTypeFuncref},
			},
			ExportSection: []*wasm.Export{
				{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
				{Name: "tab0", Type: wasm.ExternTypeTable, Index: 0},
				{Name: "tab1", Type: wasm.ExternTypeTable, Index: 1},
				{Name: "cf", Type: wasm.ExternTypeFunc, Index: 0},
			},
			StartSection: &cf,
			DataSection: []*wasm.DataSegment{
				{OffsetExpression: i32const(0), Init: []byte("CORE0CORE0")},
				{OffsetExpression: i32const(8), Init: []byte("CC")}, // later segment overrides the earlier one
			},
		}
		// core's element segments use the legacy form on tab0 and an explicit table index on tab1.
		coreElems := elementSection(
			legacyElemSegment(0, 0, 0),
			tableIndexedElemSegment(1, 1, 0),
		)
		p1 := &wasm.Module{
			TypeSection: []*wasm.FunctionType{{}},
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "core", Name: "mem", DescMem: &wasm.Memory{Min: 2, Max: 4, IsMaxEncoded: true}},
				{Type: wasm.ExternTypeTable, Module: "core", Name: "tab0", DescTable: &wasm.Table{Min: 4, Type: wasm.RefTypeFuncref}},
				{Type: wasm.ExternTypeFunc, Module: "core", Name: "cf", DescFunc: 0},
			},
			DataSection: []*wasm.DataSegment{
				{OffsetExpression: i32const(4), Init: []byte("P1P1")},
				{OffsetExpression: i32const(20), Init: []byte("DUP")},
			},
		}
		// p1's element segment writes null over core's function at slot 1, then its own import at slot 2.
		p1Elems := elementSection(constExprElemSegment(1, refNullFuncref(), refFunc(0)))
		// px only re-exports core's memory, giving p2 a two-hop alias.
		px := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "core", Name: "mem", DescMem: &wasm.Memory{Min: 2, Max: 4, IsMaxEncoded: true}},
			},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0}},
		}
		p2 := &wasm.Module{
			ImportSection: []*wasm.Import{
				{Type: wasm.ExternTypeMemory, Module: "px", Name: "mem", DescMem: &wasm.Memory{Min: 2, Max: 4, IsMaxEncoded: true}},
			},
			DataSection: []*wasm.DataSegment{
				{OffsetExpression: i32const(6), Init: []byte("P2P")},
				{OffsetExpression: i32const(20), Init: []byte("DUP")}, // identical bytes still count as a write
				{Init: []byte("PASSIVE")},                             // passive data takes no part
			},
		}
		// p2's passive and declarative element segments must not be applied.
		p2Elems := elementSection(passiveElemSegment(0), declarativeElemSegment(0))
		// big owns a 4GiB memory: zero holes of any size must stay unexpanded.
		big := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 65536},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0}},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: i32const(-268435456), Init: []byte("HI")}}, // 0xF0000000
		}
		// other exports a memory under the same name "mem": a distinct resource.
		other := &wasm.Module{
			MemorySection: &wasm.Memory{Min: 1},
			ExportSection: []*wasm.Export{{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0}},
			DataSection:   []*wasm.DataSegment{{OffsetExpression: i32const(0), Init: []byte("OT")}},
		}
		inputs := []analysis.ModuleInput{
			rawInput("core", core, coreElems),
			rawInput("p1", p1, p1Elems),
			rawInput("p2", p2, p2Elems),
			input("px", px),
			input("big", big),
			input("other", other),
		}
		b, ok := mustBundle(c, inputs, []string{"core", "p1", "p2", "px", "big", "other"})
		if !ok {
			return
		}

		// The start function is recorded, but its effects are not analyzed.
		c.Check(reflect.DeepEqual(b.Starts(), []analysis.StartFunc{{Module: "core", Index: 0}}), "starts: %v", b.Starts())

		// Same-named memories stay distinct resources.
		c.Check(reflect.DeepEqual(b.Resources(wasm.ExternTypeMemory), []analysis.ResourceID{
			{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0},
			{Module: "big", Kind: wasm.ExternTypeMemory, Index: 0},
			{Module: "other", Kind: wasm.ExternTypeMemory, Index: 0},
		}), "memories: %v", b.Resources(wasm.ExternTypeMemory))
		c.Check(reflect.DeepEqual(b.Resources(wasm.ExternTypeTable), []analysis.ResourceID{
			{Module: "core", Kind: wasm.ExternTypeTable, Index: 0},
			{Module: "core", Kind: wasm.ExternTypeTable, Index: 1},
		}), "tables: %v", b.Resources(wasm.ExternTypeTable))

		// Segments stack in initialization order; within one module, later segments override earlier ones.
		wantCore := []string{
			"[0,4)core#0+0:\"CORE\"",
			"[4,6)p1#0+0:\"P1\"",
			"[6,9)p2#0+0:\"P2P\"",
			"[9,10)core#1+1:\"C\"",
			"[10,20)zero",
			"[20,23)p2#1+0:\"DUP\"",
			"[23,131072)zero",
		}
		for _, m := range []string{"core", "p1", "p2", "px"} {
			frags, err := b.MemoryLayout(m, 0, 0, 131072)
			c.Check(err == nil, "layout via %s: %v", m, err)
			c.Check(reflect.DeepEqual(memLayoutSummary(frags), wantCore), "layout via %s:\n%v", m, strings.Join(memLayoutSummary(frags), "\n"))
		}
		// A sub-range clips the fragments it touches.
		frags, err := b.MemoryLayout("core", 0, 2, 11)
		c.Check(err == nil, "sub-range: %v", err)
		c.Check(reflect.DeepEqual(memLayoutSummary(frags), []string{
			"[2,4)core#0+2:\"RE\"", "[4,6)p1#0+0:\"P1\"", "[6,9)p2#0+0:\"P2P\"", "[9,10)core#1+1:\"C\"", "[10,11)zero",
		}), "sub-range: %v", memLayoutSummary(frags))
		// The passive data segment never appears.
		full, ok := mustMemLayout(c, b, "core", 0, 0, 131072)
		if !ok {
			return
		}
		for _, f := range memLayoutSummary(full) {
			c.Check(!strings.Contains(f, "PASSIVE"), "passive data: %s", f)
		}

		// big's 4GiB memory reports its zero holes as single fragments.
		cap, err := b.MemoryCapacity("big", 0)
		c.Check(err == nil && cap == 1<<32, "big capacity: %v %d", err, cap)
		frags, err = b.MemoryLayout("big", 0, 0, 0xF0000000)
		c.Check(err == nil, "big hole: %v", err)
		c.Check(reflect.DeepEqual(memLayoutSummary(frags), []string{"[0,4026531840)zero"}), "big hole: %v", memLayoutSummary(frags))
		frags, err = b.MemoryLayout("big", 0, 0, 1<<32)
		c.Check(err == nil, "big layout: %v", err)
		c.Check(reflect.DeepEqual(memLayoutSummary(frags), []string{
			"[0,4026531840)zero",
			"[4026531840,4026531842)big#0+0:\"HI\"",
			"[4026531842,4294967296)zero",
		}), "big layout: %v", memLayoutSummary(frags))

		// Multiple tables are tracked independently; null writes and unwritten null holes are distinct.
		tfrags, err := b.TableLayout("core", 0, 0, 4)
		c.Check(err == nil, "tab0: %v", err)
		c.Check(reflect.DeepEqual(tabLayoutSummary(tfrags), []string{
			"[0,1)core#0+0:core.func[0]",
			"[1,3)p1#0+0:null,core.func[0]",
			"[3,4)unwritten",
		}), "tab0: %v", tabLayoutSummary(tfrags))
		tfrags, err = b.TableLayout("core", 1, 0, 3)
		c.Check(err == nil, "tab1: %v", err)
		c.Check(reflect.DeepEqual(tabLayoutSummary(tfrags), []string{
			"[0,1)unwritten",
			"[1,2)core#1+0:core.func[0]",
			"[2,3)unwritten",
		}), "tab1: %v", tabLayoutSummary(tfrags))
		// p1 sees the same tab0 through its import alias, with the function identity resolved to core.
		tfrags, err = b.TableLayout("p1", 0, 0, 4)
		c.Check(err == nil, "tab0 via p1: %v", err)
		c.Check(reflect.DeepEqual(tabLayoutSummary(tfrags), []string{
			"[0,1)core#0+0:core.func[0]",
			"[1,3)p1#0+0:null,core.func[0]",
			"[3,4)unwritten",
		}), "tab0 via p1: %v", tabLayoutSummary(tfrags))

		// Audit: only core may initialize its own resources.
		rep := b.Audit(analysis.AuditPolicy{Allow: map[analysis.ResourceID][]string{
			{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0}:  {"core"},
			{Module: "core", Kind: wasm.ExternTypeTable, Index: 0}:   {"core"},
			{Module: "core", Kind: wasm.ExternTypeTable, Index: 1}:   {"core"},
			{Module: "big", Kind: wasm.ExternTypeMemory, Index: 0}:   {"big"},
			{Module: "other", Kind: wasm.ExternTypeMemory, Index: 0}: {"other"},
		}})
		c.Check(reflect.DeepEqual(writeSummary(rep.Unauthorized), []string{
			"core.memory[0] p1#0 [4,8) chain=2",
			"core.memory[0] p1#1 [20,23) chain=2",
			"core.memory[0] p2#0 [6,9) chain=3",
			"core.memory[0] p2#1 [20,23) chain=3",
			"core.table[0] p1#0 [1,3) chain=2",
		}), "unauthorized:\n%s", strings.Join(writeSummary(rep.Unauthorized), "\n"))
		// Overrides keep the exact previous source: at [8,9) that is core's second segment, not its first;
		// identical bytes (p2's "DUP" over p1's) and p1's null over core's function still count; writes into
		// zero holes produce no override.
		c.Check(reflect.DeepEqual(overrideSummary(rep.Overrides), []string{
			"core.memory[0] p1#0 [4,8) prev=core#0",
			"core.memory[0] p2#0 [6,8) prev=p1#0",
			"core.memory[0] p2#0 [8,9) prev=core#1",
			"core.memory[0] p2#1 [20,23) prev=p1#1",
			"core.table[0] p1#0 [1,2) prev=core#0",
		}), "overrides:\n%s", strings.Join(overrideSummary(rep.Overrides), "\n"))

		// Bad indices and out of bounds initialization name the module and the item.
		badtab := &wasm.Module{TableSection: []*wasm.Table{{Min: 1, Type: wasm.RefTypeFuncref}}}
		err = buildErr(
			[]analysis.ModuleInput{rawInput("badtab", badtab, elementSection(tableIndexedElemSegment(3, 0, 0)))},
			[]string{"badtab"})
		c.Check(hasErr(err, "table index 3 out of range") && hasErr(err, `"badtab"`), "bad table index: %v", err)

		badfn := &wasm.Module{
			TableSection: []*wasm.Table{{Min: 1, Type: wasm.RefTypeFuncref}},
			ElementSection: []*wasm.ElementSegment{
				{Mode: wasm.ElementModeActive, Type: wasm.RefTypeFuncref, TableIndex: 0, OffsetExpr: i32const(0), Init: []*wasm.Index{u32ptr(5)}},
			},
		}
		err = buildErr([]analysis.ModuleInput{input("badfn", badfn)}, []string{"badfn"})
		c.Check(hasErr(err, "function index 5 out of range") && hasErr(err, `"badfn"`), "bad func index: %v", err)

		badcap := &wasm.Module{
			TypeSection:     []*wasm.FunctionType{{}},
			FunctionSection: []wasm.Index{0},
			CodeSection:     []*wasm.Code{{Body: []byte{wasm.OpcodeEnd}}},
			TableSection:    []*wasm.Table{{Min: 1, Type: wasm.RefTypeFuncref}},
			ElementSection: []*wasm.ElementSegment{
				{Mode: wasm.ElementModeActive, Type: wasm.RefTypeFuncref, TableIndex: 0, OffsetExpr: i32const(0), Init: []*wasm.Index{u32ptr(0), u32ptr(0)}},
			},
		}
		err = buildErr([]analysis.ModuleInput{input("badcap", badcap)}, []string{"badcap"})
		c.Check(hasErr(err, "exceeds capacity") && hasErr(err, "element segment 0"), "table capacity: %v", err)

		// Query errors are reported, not silently clamped.
		_, err = b.MemoryLayout("core", 0, 0, 131073)
		c.Check(hasErr(err, "out of capacity"), "query past capacity: %v", err)
		_, err = b.MemoryLayout("ghost", 0, 0, 1)
		c.Check(hasErr(err, "not found"), "query unknown module: %v", err)
		_, err = b.TableLayout("core", 9, 0, 1)
		c.Check(hasErr(err, "out of range"), "query bad table: %v", err)

		// The bundle owns its data: mutating the input binaries afterwards changes nothing.
		beforeFrags, ok := mustMemLayout(c, b, "core", 0, 0, 32)
		if !ok {
			return
		}
		before := memLayoutSummary(beforeFrags)
		for _, in := range inputs {
			for i := range in.Binary {
				in.Binary[i] = 0
			}
		}
		afterFrags, ok := mustMemLayout(c, b, "core", 0, 0, 32)
		if !ok {
			return
		}
		after := memLayoutSummary(afterFrags)
		c.Check(reflect.DeepEqual(before, after), "input mutation changed the report:\n%v\n%v", before, after)
	})
}

func mustMemLayout(c *taskcheck.C, b *analysis.Bundle, module string, index uint32, start, end uint64) ([]analysis.MemoryFragment, bool) {
	frags, err := b.MemoryLayout(module, index, start, end)
	c.Check(err == nil, "MemoryLayout(%s, %d, %d, %d): %v", module, index, start, end, err)
	return frags, err == nil
}
