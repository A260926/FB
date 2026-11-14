package analysis_test

import (
	"fmt"

	"github.com/tetratelabs/wabin/analysis"
	"github.com/tetratelabs/wabin/binary"
	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// ExampleBuildBundle links a provider and a plugin that share one memory, then queries the static initial
// layout and audits which module initialized it.
func ExampleBuildBundle() {
	i32 := wasm.ValueTypeI32
	constI32 := func(v int32) *wasm.ConstantExpression {
		return &wasm.ConstantExpression{Opcode: wasm.OpcodeI32Const, Data: leb128.EncodeInt32(v)}
	}
	// core defines and exports a memory and an immutable base offset.
	core := &wasm.Module{
		MemorySection: &wasm.Memory{Min: 1, Max: 1, IsMaxEncoded: true},
		GlobalSection: []*wasm.Global{{Type: &wasm.GlobalType{ValType: i32}, Init: constI32(8)}},
		ExportSection: []*wasm.Export{
			{Name: "mem", Type: wasm.ExternTypeMemory, Index: 0},
			{Name: "base", Type: wasm.ExternTypeGlobal, Index: 0},
		},
		DataSection: []*wasm.DataSegment{{OffsetExpression: constI32(0), Init: []byte("core")}},
	}
	// plugin imports both and writes through the aliases at the imported offset.
	plugin := &wasm.Module{
		ImportSection: []*wasm.Import{
			{Type: wasm.ExternTypeMemory, Module: "core", Name: "mem", DescMem: &wasm.Memory{Min: 1, Max: 1, IsMaxEncoded: true}},
			{Type: wasm.ExternTypeGlobal, Module: "core", Name: "base", DescGlobal: &wasm.GlobalType{ValType: i32}},
		},
		DataSection: []*wasm.DataSegment{{
			OffsetExpression: &wasm.ConstantExpression{Opcode: wasm.OpcodeGlobalGet, Data: leb128.EncodeUint32(0)},
			Init:             []byte("plugin"),
		}},
	}
	bundle, err := analysis.BuildBundle([]analysis.ModuleInput{
		{Name: "core", Binary: binary.EncodeModule(core)},
		{Name: "plugin", Binary: binary.EncodeModule(plugin)},
	}, []string{"core", "plugin"})
	if err != nil {
		panic(err)
	}

	res, err := bundle.Resolve("plugin", wasm.ExternTypeMemory, 0)
	if err != nil {
		panic(err)
	}
	fmt.Println("plugin memory is defined by:", res.Final)

	frags, err := bundle.MemoryLayout("core", 0, 0, 16)
	if err != nil {
		panic(err)
	}
	for _, f := range frags {
		if f.Written {
			fmt.Printf("[%d,%d) %q written by %s segment %d\n", f.Start, f.End, f.Data, f.Module, f.Segment)
		} else {
			fmt.Printf("[%d,%d) zero\n", f.Start, f.End)
		}
	}

	report := bundle.Audit(analysis.AuditPolicy{Allow: map[analysis.ResourceID][]string{
		{Module: "core", Kind: wasm.ExternTypeMemory, Index: 0}: {"core"},
	}})
	for _, u := range report.Unauthorized {
		fmt.Printf("unauthorized: %s wrote %s [%d,%d) via %d-hop alias\n", u.Writer, u.Resource, u.Start, u.End, len(u.Chain)-1)
	}

	// Output:
	// plugin memory is defined by: core.memory[0]
	// [0,4) "core" written by core segment 0
	// [4,8) zero
	// [8,14) "plugin" written by plugin segment 0
	// [14,16) zero
	// unauthorized: plugin wrote core.memory[0] [8,14) via 1-hop alias
}
