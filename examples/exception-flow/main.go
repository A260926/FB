/*
	Copyright 2023 Loophole Labs

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		   http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
*/

// Command exception-flow reads a WebAssembly image and prints a static,
// read-only exception-flow report for the try_table based exception handling
// proposal. It never instantiates the module, never calls imports, and never
// writes the module back.
//
// When --isolated and --boundary are supplied it also prints a static plugin
// boundary policy audit for host functions that must be isolated. That audit
// is a syntactic policy result: it is not an observation of a runtime
// exception, and it must not be read as evidence that an attack occurred.
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"
	"strings"

	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/expression"
	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/types"
	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/wasmfile"
)

// stringList collects a repeatable string flag in order.
type stringList []string

func (s *stringList) String() string { return strings.Join(*s, ",") }

func (s *stringList) Set(v string) error {
	*s = append(*s, v)
	return nil
}

// intList collects a repeatable integer flag in order.
type intList []int

func (l *intList) String() string {
	parts := make([]string, len(*l))
	for i, v := range *l {
		parts[i] = fmt.Sprintf("%d", v)
	}
	return strings.Join(parts, ",")
}

func (l *intList) Set(v string) error {
	var n int
	if _, err := fmt.Sscanf(v, "%d", &n); err != nil {
		return fmt.Errorf("invalid integer %q", v)
	}
	*l = append(*l, n)
	return nil
}

func main() {
	var isolated stringList
	var boundaries intList
	flag.Var(&isolated, "isolated", "isolated host function import, as module:name or name (repeatable)")
	flag.Var(&boundaries, "boundary", "defined function index used as a plugin boundary (repeatable)")
	flag.Usage = func() {
		fmt.Fprintf(flag.CommandLine.Output(), "usage: %s [flags] <module.wasm>\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() != 1 {
		flag.Usage()
		os.Exit(2)
	}

	image, err := os.ReadFile(flag.Arg(0))
	if err != nil {
		fmt.Fprintf(os.Stderr, "read module: %v\n", err)
		os.Exit(1)
	}

	wf := wasmfile.NewEmpty()
	if err := wf.DecodeBinary(image); err != nil {
		fmt.Fprintf(os.Stderr, "decode module: %v\n", err)
		os.Exit(1)
	}

	printModuleFacts(wf)

	flow, err := wf.BuildExceptionFlow()
	if err != nil {
		fmt.Fprintf(os.Stderr, "analyze exception flow: %v\n", err)
		os.Exit(1)
	}
	printFlow(flow)

	if len(isolated) > 0 || len(boundaries) > 0 {
		if len(isolated) == 0 || len(boundaries) == 0 {
			fmt.Fprintln(os.Stderr, "policy audit needs at least one --isolated and one --boundary")
			os.Exit(2)
		}
		audit, err := wf.AuditPluginBoundary(&wasmfile.PluginBoundaryPolicy{
			IsolatedImports: []string(isolated),
			Boundaries:      []int(boundaries),
		})
		if err != nil {
			fmt.Fprintf(os.Stderr, "audit plugin boundary: %v\n", err)
			os.Exit(1)
		}
		printAudit(audit)
	}
}

func printModuleFacts(wf *wasmfile.WasmFile) {
	fmt.Println("== module ==")
	fmt.Printf("function imports: %d\n", wf.FunctionImportCount())
	fmt.Printf("tag imports: %d\n", len(wf.TagImport))
	fmt.Printf("defined tags: %d\n", len(wf.Tag))
	fmt.Printf("declared tags: %d\n", len(wf.TagImport)+len(wf.Tag))
	fmt.Printf("defined functions: %d\n", len(wf.Code))

	for i, t := range wf.TagImport {
		fmt.Printf("tag %d: imported %s:%s type %d\n", i, t.Module, t.Name, t.Index)
	}
	for i, t := range wf.Tag {
		fmt.Printf("tag %d: defined type %d attribute %d\n", len(wf.TagImport)+i, t.TypeIndex, t.Attribute)
	}

	exports := make([]*wasmfile.ExportEntry, 0)
	for _, e := range wf.Export {
		if e.Type == types.ExportTag {
			exports = append(exports, e)
		}
	}
	sort.SliceStable(exports, func(i, j int) bool { return exports[i].Name < exports[j].Name })
	for _, e := range exports {
		fmt.Printf("tag export %q: tag %d\n", e.Name, e.Index)
	}
	fmt.Println()
}

// pcSpan formats an excerpt of a stable transfer listing without hiding the
// original PCs.
func pcSpan(pc, pcNext uint64) string {
	return fmt.Sprintf("[%d,%d)", pc, pcNext)
}

func identityString(id wasmfile.ExceptionIdentity) string {
	if id.Foreign {
		return "foreign"
	}
	return fmt.Sprintf("tag %d", id.TagIndex)
}

func kindString(k expression.CatchKind) string {
	switch k {
	case expression.CatchTag:
		return "catch"
	case expression.CatchTagRef:
		return "catch_ref"
	case expression.CatchAll:
		return "catch_all"
	case expression.CatchAllRef:
		return "catch_all_ref"
	default:
		return fmt.Sprintf("catch-kind(%d)", byte(k))
	}
}

func catchOrderString(order []expression.Catch) string {
	parts := make([]string, 0, len(order))
	for _, c := range order {
		switch c.Kind {
		case expression.CatchTag:
			parts = append(parts, fmt.Sprintf("catch tag %d -> label %d", c.TagIndex, c.LabelIndex))
		case expression.CatchTagRef:
			parts = append(parts, fmt.Sprintf("catch_ref tag %d -> label %d", c.TagIndex, c.LabelIndex))
		case expression.CatchAll:
			parts = append(parts, fmt.Sprintf("catch_all -> label %d", c.LabelIndex))
		case expression.CatchAllRef:
			parts = append(parts, fmt.Sprintf("catch_all_ref -> label %d", c.LabelIndex))
		}
	}
	return strings.Join(parts, ", ")
}

func targetString(t wasmfile.ExceptionTarget) string {
	switch {
	case t.Escapes:
		return "escapes function"
	case t.FunctionExit:
		return "caught, function exit"
	default:
		return fmt.Sprintf("caught, continue at pc %d", t.PC)
	}
}

func printFlow(flow *wasmfile.ExceptionFlow) {
	fmt.Println("== function escape summaries ==")
	for _, f := range flow.Functions {
		tags := make([]string, 0, len(f.EscapingTags))
		for _, t := range f.EscapingTags {
			tags = append(tags, fmt.Sprintf("%d", t))
		}
		tagText := "none"
		if len(tags) > 0 {
			tagText = strings.Join(tags, ",")
		}
		fmt.Printf("function %d: escaping tags [%s] foreign %t\n", f.FunctionIndex, tagText, f.Foreign)
	}
	fmt.Println()

	fmt.Println("== exception migrations ==")
	if len(flow.Transfers) == 0 {
		fmt.Println("none")
		fmt.Println()
		return
	}
	for _, t := range flow.Transfers {
		site := t.Site.String()
		switch t.Site {
		case wasmfile.ExceptionSiteCall:
			site = fmt.Sprintf("call func %d", t.Callee)
		case wasmfile.ExceptionSiteCallIndirect:
			site = fmt.Sprintf("call_indirect type %d", t.TypeIndex)
		}
		handler := "no handler"
		if t.Handler != nil {
			handler = fmt.Sprintf("try_table %s catch #%d %s tag %d -> label %d [%s]",
				pcSpan(t.Handler.TryTablePC, t.Handler.TryTablePCNext),
				t.Handler.CatchIndex,
				kindString(t.Handler.Kind),
				t.Handler.TagIndex,
				t.Handler.LabelIndex,
				catchOrderString(t.Handler.CatchOrder))
		}
		fmt.Printf("function %d: %s at pc %s identity %s: %s; target %s\n",
			t.FunctionIndex, site, pcSpan(t.SourcePC, t.SourcePCNext),
			identityString(t.Identity), handler, targetString(t.Target))
	}
	fmt.Println()
}

func printAudit(audit *wasmfile.PluginBoundaryAudit) {
	fmt.Println("== static plugin boundary policy ==")
	fmt.Println("This is a syntactic boundary policy result, not a runtime observation;")
	fmt.Println("it does not claim that an exception or attack occurred.")
	fmt.Printf("isolated imports: %s\n", strings.Join(audit.IsolatedImports, ", "))
	fmt.Printf("overall: %s\n", audit.Status)
	for _, reason := range audit.InsufficientReasons {
		fmt.Printf("insufficient: %s\n", reason)
	}
	for _, b := range audit.Boundaries {
		fmt.Printf("boundary function %d: %s\n", b.FunctionIndex, b.Status)
		if len(b.Findings) == 0 {
			fmt.Println("  no isolated host-call sites")
			continue
		}
		for _, f := range b.Findings {
			handler := "uncaught"
			if f.Handler != nil {
				handler = fmt.Sprintf("try_table %s catch #%d %s tag %d -> label %d",
					pcSpan(f.Handler.TryTablePC, f.Handler.TryTablePCNext),
					f.Handler.CatchIndex,
					kindString(f.Handler.Kind),
					f.Handler.TagIndex,
					f.Handler.LabelIndex)
			}
			fmt.Printf("  %s:%s (func %d) at pc %s identity %s: %s; %s\n",
				f.ImportModule, f.ImportName, f.ImportFunctionIndex,
				pcSpan(f.SourcePC, f.SourcePCNext), identityString(f.Identity),
				handler, targetString(f.Target))
		}
	}
	fmt.Println()
}
