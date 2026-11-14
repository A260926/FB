package analysis_test

import (
	"fmt"

	"github.com/tetratelabs/wabin/analysis"
	"github.com/tetratelabs/wabin/wasm"
)

// ExampleAuditHostCapabilities statically audits a Wasm plugin's direct host
// calls against a caller-provided allowlist, without instantiating the
// module or executing any import.
func ExampleAuditHostCapabilities() {
	i32 := wasm.ValueTypeI32
	m := &wasm.Module{
		TypeSection: []*wasm.FunctionType{
			{Params: []wasm.ValueType{i32}}, // 0: host(i32)
			{},                              // 1: () -> ()
		},
		ImportSection: []*wasm.Import{
			{Type: wasm.ExternTypeFunc, Module: "env", Name: "log", DescFunc: 0},
			{Type: wasm.ExternTypeFunc, Module: "env", Name: "exec", DescFunc: 0},
		},
		FunctionSection: []wasm.Index{1},
		CodeSection: []*wasm.Code{{Body: []byte{
			wasm.OpcodeI32Const, 7,
			wasm.OpcodeCall, 0, // env.log(7): allowlisted
			wasm.OpcodeI32Const, 1,
			wasm.OpcodeCall, 1, // env.exec(1): not allowlisted
			wasm.OpcodeEnd,
		}}},
	}
	report := analysis.AuditHostCapabilities(m, []analysis.HostFuncRef{{Module: "env", Name: "log"}})
	for _, c := range report.Calls {
		fmt.Printf("call %s.%s func=%d offset=%d allowed=%v args=%d\n",
			c.Module, c.Name, c.FuncIndex, c.Offset, c.Allowed, len(c.Args))
	}
	fmt.Println("admissible:", report.Admissible())
	// Output:
	// call env.log func=2 offset=2 allowed=true args=1
	// call env.exec func=2 offset=6 allowed=false args=1
	// admissible: false
}
