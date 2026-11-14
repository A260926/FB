package analysis

import (
	"github.com/tetratelabs/wabin/wasm"
)

// HostFuncRef identifies an imported host function by its two-level
// namespace, used as an allowlist entry for AuditHostCapabilities.
type HostFuncRef struct {
	Module string
	Name   string
}

// ArgSource reports the origins of one call argument, traced backwards
// through block parameter edges only. Origins that pass through local.get
// are flagged External: no cross-block local variable dataflow is inferred.
type ArgSource struct {
	// Arg is the argument value node at the call site.
	Arg *Value
	// Origins are the leaf value nodes reachable from Arg through block
	// parameter edges, sorted by (Offset, Kind, Index). Distinct incoming
	// edges keep their distinct argument identities.
	Origins []*Value
	// External is true when any origin is a local.get external source.
	External bool
}

// HostCall describes one reachable direct call to an imported function.
type HostCall struct {
	// FuncIndex is the analyzed function containing the call.
	FuncIndex wasm.Index
	// Offset is the Body offset of the call instruction.
	Offset uint32
	// Module and Name identify the imported callee.
	Module string
	Name   string
	// CallIndex is the callee's function index.
	CallIndex wasm.Index
	// Allowed is true when the callee is in the caller-provided allowlist.
	// A call outside the list is a policy rejection.
	Allowed bool
	// Args are the argument value nodes at the call site, in declared order.
	Args []*Value
	// Sources traces each argument through block parameter edges, parallel
	// to Args.
	Sources []ArgSource
}

// FunctionAudit records whether one local function could be analyzed.
type FunctionAudit struct {
	FuncIndex wasm.Index
	// Complete is false when analysis failed, e.g. on an unsupported
	// instruction. An incomplete function can never yield an allow verdict.
	Complete bool
	Err      error
}

// PolicyReport is the result of a static admission audit of a Wasm plugin's
// host capability boundary. It covers code admission only: it does not claim
// any exploit chain.
type PolicyReport struct {
	// Functions audits every local function, sorted by function index.
	Functions []FunctionAudit
	// Calls lists every reachable direct host call, sorted by (FuncIndex,
	// Offset). Unreachable calls produce no entries.
	Calls []HostCall
}

// Admissible reports whether the plugin may be admitted: every local
// function analyzed completely and every reachable host call is allowed.
func (r *PolicyReport) Admissible() bool {
	for _, f := range r.Functions {
		if !f.Complete {
			return false
		}
	}
	for _, c := range r.Calls {
		if !c.Allowed {
			return false
		}
	}
	return true
}

// AuditHostCapabilities statically audits which imported host functions a
// plugin's local functions call directly, consuming AnalyzeStackTransfers
// results. allowed is the caller-provided allowlist of host imports.
//
// The audit never instantiates the module and never executes imports.
func AuditHostCapabilities(m *wasm.Module, allowed []HostFuncRef) *PolicyReport {
	allow := make(map[HostFuncRef]bool, len(allowed))
	for _, ref := range allowed {
		allow[ref] = true
	}
	var funcImports []*wasm.Import
	for _, imp := range m.ImportSection {
		if imp != nil && imp.Type == wasm.ExternTypeFunc {
			funcImports = append(funcImports, imp)
		}
	}
	report := &PolicyReport{}
	numFuncImports := wasm.Index(len(funcImports))
	for def := 0; def < len(m.FunctionSection) && def < len(m.CodeSection); def++ {
		funcIndex := numFuncImports + wasm.Index(def)
		g, err := AnalyzeStackTransfers(m, funcIndex)
		if err != nil {
			report.Functions = append(report.Functions, FunctionAudit{FuncIndex: funcIndex, Complete: false, Err: err})
			continue
		}
		report.Functions = append(report.Functions, FunctionAudit{FuncIndex: funcIndex, Complete: true})
		report.Calls = append(report.Calls, auditCalls(g, funcImports, allow)...)
	}
	return report
}

// auditCalls extracts the reachable direct host calls of one analyzed
// function.
func auditCalls(g *Graph, funcImports []*wasm.Import, allow map[HostFuncRef]bool) []HostCall {
	var calls []HostCall
	for _, instr := range g.Instructions {
		if instr.Opcode != wasm.OpcodeCall {
			continue
		}
		callee := instr.CallTarget
		if int(callee) >= len(funcImports) {
			continue // a local call is not a host capability.
		}
		imp := funcImports[callee]
		call := HostCall{
			FuncIndex: g.FuncIndex,
			Offset:    instr.Offset,
			Module:    imp.Module,
			Name:      imp.Name,
			CallIndex: callee,
			Allowed:   allow[HostFuncRef{Module: imp.Module, Name: imp.Name}],
			Args:      instr.Inputs,
		}
		for _, arg := range instr.Inputs {
			call.Sources = append(call.Sources, traceArg(g, arg))
		}
		calls = append(calls, call)
	}
	return calls
}

// traceArg traces one argument backwards through block parameter edges to
// its leaf origins. Loop back edges can form cycles, so visited nodes are
// traced once.
func traceArg(g *Graph, arg *Value) ArgSource {
	inEdges := map[*Value][]*Edge{}
	for _, e := range g.Edges {
		for _, t := range e.Targets {
			inEdges[t] = append(inEdges[t], e)
		}
	}
	origins := map[*Value]bool{}
	visited := map[*Value]bool{}
	var walk func(v *Value)
	walk = func(v *Value) {
		if visited[v] {
			return
		}
		visited[v] = true
		switch v.Kind {
		case ValueBlockParam, ValueBlockResult, ValueFuncResult:
			edges := inEdges[v]
			if len(edges) == 0 {
				origins[v] = true
				return
			}
			for _, e := range edges {
				for i, t := range e.Targets {
					if t == v {
						walk(e.Args[i])
					}
				}
			}
		default:
			// Instruction results and local.get external sources are leaves
			// of the block-parameter trace.
			origins[v] = true
		}
	}
	src := ArgSource{Arg: arg}
	if arg != nil {
		walk(arg)
	}
	for v := range origins {
		src.Origins = append(src.Origins, v)
		if v.Kind == ValueLocalGet {
			src.External = true
		}
	}
	sortValues(src.Origins)
	return src
}
