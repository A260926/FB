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

package wasmfile

import (
	"fmt"
	"sort"
	"strings"

	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/expression"
	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/types"
)

// This file implements a read-only, syntactic exception-flow analysis for the
// final (try_table based) WebAssembly exception handling proposal. Nothing is
// instantiated, called, or modified; the analysis only inspects the decoded
// instruction stream.
//
// The analysis is deliberately path-insensitive: every decoded throw,
// throw_ref, call and call_indirect is considered, including instructions
// that follow an unconditional transfer. No operand or reachability analysis
// is performed, and a trap is not an exception. Recursive call cycles are
// resolved with a least fixed point over function escaping summaries, so a
// cycle never widens to "any exception".

// ExceptionSite is the kind of instruction that can start an exception
// transfer.
type ExceptionSite byte

const (
	// ExceptionSiteThrow is throw <tag>.
	ExceptionSiteThrow ExceptionSite = iota
	// ExceptionSiteThrowRef is throw_ref, which can rethrow any exception
	// reference and therefore any declared tag or a module-external one.
	ExceptionSiteThrowRef
	// ExceptionSiteCall is call <func>.
	ExceptionSiteCall
	// ExceptionSiteCallIndirect is call_indirect <type> <table>.
	ExceptionSiteCallIndirect
)

func (s ExceptionSite) String() string {
	switch s {
	case ExceptionSiteThrow:
		return "throw"
	case ExceptionSiteThrowRef:
		return "throw_ref"
	case ExceptionSiteCall:
		return "call"
	case ExceptionSiteCallIndirect:
		return "call_indirect"
	default:
		return fmt.Sprintf("site(%d)", byte(s))
	}
}

// ExceptionIdentity identifies what a possible exception is. Identities are
// the module's tag indices (imported tags first, then tag-section
// definitions); tags with equal signatures are never merged. Foreign means an
// exception that did not originate from a tag declared by this module. No
// alias between imported tags is ever inferred.
type ExceptionIdentity struct {
	// Foreign reports a module-external exception.
	Foreign bool
	// TagIndex is the module tag index when Foreign is false, and -1 when
	// Foreign is true.
	TagIndex int
}

// TagException returns the identity of a declared module tag.
func TagException(tagIndex int) ExceptionIdentity {
	return ExceptionIdentity{TagIndex: tagIndex}
}

// ForeignException returns the identity of a module-external exception.
func ForeignException() ExceptionIdentity {
	return ExceptionIdentity{Foreign: true, TagIndex: -1}
}

func (id ExceptionIdentity) String() string {
	if id.Foreign {
		return "foreign"
	}
	return fmt.Sprintf("tag:%d", id.TagIndex)
}

// ExceptionHandler describes the try_table catch clause that consumed a
// possible exception. CatchOrder keeps the full encoded catch vector so a
// consumer can see the complete ordering that was searched.
type ExceptionHandler struct {
	// TryTablePC and TryTablePCNext are the original program counters of the
	// try_table instruction; TryTablePCNext is exclusive.
	TryTablePC     uint64
	TryTablePCNext uint64
	// CatchOrder is the complete catch vector in encoded order.
	CatchOrder []expression.Catch
	// CatchIndex indexes CatchOrder; the first matching clause wins.
	CatchIndex int
	// Kind, TagIndex and LabelIndex repeat the matched clause. TagIndex is
	// -1 for catch_all and catch_all_ref.
	Kind       expression.CatchKind
	TagIndex   int
	LabelIndex int
}

// ExceptionTarget describes where an exception goes after the catch search.
type ExceptionTarget struct {
	// Escapes reports that no enclosing try_table caught the identity and it
	// propagates out of the function.
	Escapes bool
	// FunctionExit reports that a catch branched to the function label, so
	// the exception was consumed and the function returns normally.
	FunctionExit bool
	// PC is the continuation program counter when the exception was caught
	// by a block/loop/if/try_table label (not the function label).
	PC uint64
}

// ExceptionTransfer is one possible exception migration from one instruction
// site for one identity. A site with several possible identities produces one
// transfer per identity, each with its own catch search result.
type ExceptionTransfer struct {
	// FunctionIndex is the module function index of the containing defined
	// function (only function imports form its prefix).
	FunctionIndex int
	Site          ExceptionSite
	// SourcePC and SourcePCNext preserve the original instruction span;
	// SourcePCNext is exclusive.
	SourcePC     uint64
	SourcePCNext uint64
	// Identity is the single possible exception identity of this transfer.
	Identity ExceptionIdentity
	// Callee is the direct call target function index for ExceptionSiteCall,
	// and -1 otherwise.
	Callee int
	// TypeIndex is the call_indirect type index for
	// ExceptionSiteCallIndirect, and -1 otherwise.
	TypeIndex int
	// Handler is nil when the identity escapes the function.
	Handler *ExceptionHandler
	Target  ExceptionTarget
}

// FunctionExceptionSummary is the exception escape summary of one defined
// function: the declared tag identities plus the module-external possibility
// that may leave the function.
type FunctionExceptionSummary struct {
	FunctionIndex int
	EscapingTags  []int
	Foreign       bool
}

// ExceptionFlow is the result of BuildExceptionFlow.
type ExceptionFlow struct {
	// FunctionImportCount is the number of function imports. Only function
	// imports form the function index prefix; tag imports never consume it.
	FunctionImportCount int
	TagImportCount      int
	// DeclaredTagCount is the module tag index space size: imported tags
	// followed by tag-section definitions.
	DeclaredTagCount int
	// Functions holds one escape summary per defined function, in function
	// index order.
	Functions []*FunctionExceptionSummary
	// Transfers holds all exception migrations, ordered stably by function
	// index, source program counter, site kind and identity.
	Transfers []*ExceptionTransfer
}

// flowLabelKind is the kind of syntactic label pushed on the label stack.
type flowLabelKind byte

const (
	flowLabelFunction flowLabelKind = iota
	flowLabelBlock
	flowLabelLoop
	flowLabelIf
	flowLabelTryTable
)

// flowLabel is one entry of the label environment. Depth zero is the
// innermost label. A loop's branch continuation is its body entry; block, if
// and try_table continuations are the point after their matching end.
type flowLabel struct {
	kind        flowLabelKind
	loopEntryPC uint64
	endNextPC   uint64
}

// flowCatcher is one resolved catch clause. targetLabel is nil when the label
// depth names the function label (the function exit).
type flowCatcher struct {
	clause      expression.Catch
	targetLabel *flowLabel
}

// flowTryFrame is a try_table whose body encloses some instruction.
type flowTryFrame struct {
	expr    *expression.Expression
	catches []flowCatcher
}

// flowInstr records the enclosing try_table frames for one instruction, from
// innermost to outermost.
type flowInstr struct {
	tryFrames []*flowTryFrame
}

// flowState is a set of exception identities.
type flowState struct {
	tags    map[int]struct{}
	foreign bool
}

func (s *flowState) add(id ExceptionIdentity) {
	if id.Foreign {
		s.foreign = true
		return
	}
	if s.tags == nil {
		s.tags = make(map[int]struct{})
	}
	s.tags[id.TagIndex] = struct{}{}
}

// merge unions o into s and reports whether s changed.
func (s *flowState) merge(o *flowState) bool {
	changed := false
	if o.foreign && !s.foreign {
		s.foreign = true
		changed = true
	}
	for t := range o.tags {
		if s.tags == nil {
			s.tags = make(map[int]struct{})
		}
		if _, ok := s.tags[t]; !ok {
			s.tags[t] = struct{}{}
			changed = true
		}
	}
	return changed
}

func (s *flowState) sortedTags() []int {
	tags := make([]int, 0, len(s.tags))
	for t := range s.tags {
		tags = append(tags, t)
	}
	sort.Ints(tags)
	return tags
}

func (s *flowState) identities() []ExceptionIdentity {
	ids := make([]ExceptionIdentity, 0, len(s.tags)+1)
	for _, t := range s.sortedTags() {
		ids = append(ids, TagException(t))
	}
	if s.foreign {
		ids = append(ids, ForeignException())
	}
	return ids
}

// declaredTagCount is the size of the module tag index space.
func (wf *WasmFile) declaredTagCount() int {
	return len(wf.TagImport) + len(wf.Tag)
}

// allExceptionIdentities yields every declared tag identity in ascending
// index order followed by the module-external possibility.
func allExceptionIdentities(tagCount int) []ExceptionIdentity {
	ids := make([]ExceptionIdentity, 0, tagCount+1)
	for t := 0; t < tagCount; t++ {
		ids = append(ids, TagException(t))
	}
	ids = append(ids, ForeignException())
	return ids
}

// buildFlowStructure resolves the label environment of one function. Catch
// labels are resolved before the try_table's own body label is added, so a
// catch depth is interpreted in the outer label context. Label numbers are
// never treated as instruction indices.
func buildFlowStructure(exprs []*expression.Expression, functionIndex int, tagCount int) ([]flowInstr, error) {
	infos := make([]flowInstr, len(exprs))
	labels := []*flowLabel{{kind: flowLabelFunction}}
	var tryStack []*flowTryFrame

	for i, e := range exprs {
		// Snapshot the enclosing try_table frames before any structural
		// change caused by this instruction.
		if len(tryStack) > 0 {
			frames := make([]*flowTryFrame, len(tryStack))
			for n := range tryStack {
				frames[len(tryStack)-1-n] = tryStack[n]
			}
			infos[i].tryFrames = frames
		}

		switch e.Opcode {
		case expression.InstrToOpcode["try_table"]:
			tf := &flowTryFrame{expr: e}
			for _, c := range e.Catches {
				if c.Kind == expression.CatchTag || c.Kind == expression.CatchTagRef {
					if c.TagIndex < 0 || c.TagIndex >= tagCount {
						return nil, fmt.Errorf("exception flow: function %d offset %d: invalid catch tag %d (declared tags %d)", functionIndex, e.PC, c.TagIndex, tagCount)
					}
				}
				if c.LabelIndex < 0 || c.LabelIndex >= len(labels) {
					return nil, fmt.Errorf("exception flow: function %d offset %d: invalid catch label depth %d", functionIndex, e.PC, c.LabelIndex)
				}
				target := labels[len(labels)-1-c.LabelIndex]
				fc := flowCatcher{clause: c}
				if target.kind != flowLabelFunction {
					fc.targetLabel = target
				}
				tf.catches = append(tf.catches, fc)
			}
			labels = append(labels, &flowLabel{kind: flowLabelTryTable})
			tryStack = append(tryStack, tf)
		case expression.InstrToOpcode["block"]:
			labels = append(labels, &flowLabel{kind: flowLabelBlock})
		case expression.InstrToOpcode["loop"]:
			labels = append(labels, &flowLabel{kind: flowLabelLoop, loopEntryPC: e.PCNext})
		case expression.InstrToOpcode["if"]:
			labels = append(labels, &flowLabel{kind: flowLabelIf})
		case expression.InstrToOpcode["end"]:
			if len(labels) == 0 || labels[len(labels)-1].kind == flowLabelFunction {
				return nil, fmt.Errorf("exception flow: function %d offset %d: unbalanced end", functionIndex, e.PC)
			}
			f := labels[len(labels)-1]
			f.endNextPC = e.PCNext
			labels = labels[:len(labels)-1]
			if f.kind == flowLabelTryTable {
				tryStack = tryStack[:len(tryStack)-1]
			}
		}
	}

	if len(labels) != 1 || len(tryStack) != 0 {
		return nil, fmt.Errorf("exception flow: function %d: unbalanced block structure", functionIndex)
	}
	return infos, nil
}

// catcherMatches applies the encoded catch semantics: catch/catch_ref match
// one tag identity, while catch_all/catch_all_ref also match module-external
// exceptions.
func catcherMatches(c expression.Catch, id ExceptionIdentity) bool {
	switch c.Kind {
	case expression.CatchTag, expression.CatchTagRef:
		return !id.Foreign && id.TagIndex == c.TagIndex
	case expression.CatchAll, expression.CatchAllRef:
		return true
	default:
		return false
	}
}

// catcherTarget converts a resolved catch target into an ExceptionTarget.
func catcherTarget(fc *flowCatcher) ExceptionTarget {
	if fc.targetLabel == nil {
		return ExceptionTarget{FunctionExit: true}
	}
	f := fc.targetLabel
	if f.kind == flowLabelLoop {
		return ExceptionTarget{PC: f.loopEntryPC}
	}
	return ExceptionTarget{PC: f.endNextPC}
}

func newHandler(tf *flowTryFrame, catchIndex int) *ExceptionHandler {
	c := tf.catches[catchIndex]
	order := make([]expression.Catch, len(tf.expr.Catches))
	copy(order, tf.expr.Catches)
	return &ExceptionHandler{
		TryTablePC:     tf.expr.PC,
		TryTablePCNext: tf.expr.PCNext,
		CatchOrder:     order,
		CatchIndex:     catchIndex,
		Kind:           c.clause.Kind,
		TagIndex:       c.clause.TagIndex,
		LabelIndex:     c.clause.LabelIndex,
	}
}

// scheduleIdentity searches the enclosing try_table frames from innermost to
// outermost and returns the first matching clause and its target. A nil
// handler means the identity escapes the function.
func scheduleIdentity(frames []*flowTryFrame, id ExceptionIdentity) (*ExceptionHandler, ExceptionTarget) {
	for _, tf := range frames {
		for ci, fc := range tf.catches {
			if catcherMatches(fc.clause, id) {
				return newHandler(tf, ci), catcherTarget(&fc)
			}
		}
	}
	return nil, ExceptionTarget{Escapes: true}
}

func (wf *WasmFile) siteIdentities(e *expression.Expression, functionIndex int, states []*flowState, prefix int, tagCount int) (ExceptionSite, []ExceptionIdentity, bool, error) {
	switch e.Opcode {
	case expression.InstrToOpcode["throw"]:
		if e.TagIndex < 0 || e.TagIndex >= tagCount {
			return 0, nil, false, fmt.Errorf("exception flow: function %d offset %d: invalid throw tag %d (declared tags %d)", functionIndex, e.PC, e.TagIndex, tagCount)
		}
		return ExceptionSiteThrow, []ExceptionIdentity{TagException(e.TagIndex)}, true, nil
	case expression.InstrToOpcode["throw_ref"]:
		return ExceptionSiteThrowRef, allExceptionIdentities(tagCount), true, nil
	case expression.InstrToOpcode["call"]:
		bound := prefix + len(wf.Code)
		if e.FuncIndex < 0 || e.FuncIndex >= bound {
			return 0, nil, false, fmt.Errorf("exception flow: function %d offset %d: invalid call target %d", functionIndex, e.PC, e.FuncIndex)
		}
		if e.FuncIndex < prefix {
			// Imported (host) function: any declared tag or foreign.
			return ExceptionSiteCall, allExceptionIdentities(tagCount), true, nil
		}
		st := states[e.FuncIndex-prefix]
		return ExceptionSiteCall, st.identities(), true, nil
	case expression.InstrToOpcode["call_indirect"]:
		return ExceptionSiteCallIndirect, allExceptionIdentities(tagCount), true, nil
	default:
		return 0, nil, false, nil
	}
}

// functionEscapes computes the identities that escape one function given the
// current callee summaries. It is monotone in the summaries, which is what
// makes the fixed point reach the least solution.
func (wf *WasmFile) functionEscapes(functionIndex int, structure []flowInstr, exprs []*expression.Expression, states []*flowState, prefix int, tagCount int) (*flowState, error) {
	esc := &flowState{}
	for i, e := range exprs {
		_, ids, ok, err := wf.siteIdentities(e, functionIndex, states, prefix, tagCount)
		if err != nil {
			return nil, err
		}
		if !ok {
			continue
		}
		for _, id := range ids {
			if h, _ := scheduleIdentity(structure[i].tryFrames, id); h == nil {
				esc.add(id)
			}
		}
	}
	return esc, nil
}

// functionTransfers emits the transfer records of one function using the
// converged callee summaries.
func (wf *WasmFile) functionTransfers(functionIndex int, structure []flowInstr, exprs []*expression.Expression, states []*flowState, prefix int, tagCount int) ([]*ExceptionTransfer, error) {
	transfers := make([]*ExceptionTransfer, 0)
	for i, e := range exprs {
		site, ids, ok, err := wf.siteIdentities(e, functionIndex, states, prefix, tagCount)
		if err != nil {
			return nil, err
		}
		if !ok {
			continue
		}
		for _, id := range ids {
			t := &ExceptionTransfer{
				FunctionIndex: functionIndex,
				Site:          site,
				SourcePC:      e.PC,
				SourcePCNext:  e.PCNext,
				Identity:      id,
				Callee:        -1,
				TypeIndex:     -1,
			}
			if site == ExceptionSiteCall {
				t.Callee = e.FuncIndex
			}
			if site == ExceptionSiteCallIndirect {
				t.TypeIndex = e.TypeIndex
			}
			if h, target := scheduleIdentity(structure[i].tryFrames, id); h != nil {
				t.Handler = h
				t.Target = target
			} else {
				t.Target = ExceptionTarget{Escapes: true}
			}
			transfers = append(transfers, t)
		}
	}
	return transfers, nil
}

// BuildExceptionFlow analyzes the decoded module and returns per-function
// escape summaries plus one transfer record per possible identity of every
// throw, throw_ref, call and call_indirect. The receiver is not modified.
func (wf *WasmFile) BuildExceptionFlow() (*ExceptionFlow, error) {
	prefix := wf.FunctionImportCount()
	tagCount := wf.declaredTagCount()
	n := len(wf.Code)

	structures := make([][]flowInstr, n)
	for d, c := range wf.Code {
		infos, err := buildFlowStructure(c.Expression, prefix+d, tagCount)
		if err != nil {
			return nil, err
		}
		structures[d] = infos
	}

	states := make([]*flowState, n)
	for i := range states {
		states[i] = &flowState{}
	}

	// Least fixed point over function escape summaries. Starting from empty
	// summaries keeps recursive cycles minimal; only real syntactic sources
	// contribute identities.
	for {
		changed := false
		for d, c := range wf.Code {
			esc, err := wf.functionEscapes(prefix+d, structures[d], c.Expression, states, prefix, tagCount)
			if err != nil {
				return nil, err
			}
			if states[d].merge(esc) {
				changed = true
			}
		}
		if !changed {
			break
		}
	}

	flow := &ExceptionFlow{
		FunctionImportCount: prefix,
		TagImportCount:      len(wf.TagImport),
		DeclaredTagCount:    tagCount,
	}
	for d, c := range wf.Code {
		flow.Functions = append(flow.Functions, &FunctionExceptionSummary{
			FunctionIndex: prefix + d,
			EscapingTags:  states[d].sortedTags(),
			Foreign:       states[d].foreign,
		})
		transfers, err := wf.functionTransfers(prefix+d, structures[d], c.Expression, states, prefix, tagCount)
		if err != nil {
			return nil, err
		}
		flow.Transfers = append(flow.Transfers, transfers...)
	}

	sort.SliceStable(flow.Transfers, func(i, j int) bool {
		a, b := flow.Transfers[i], flow.Transfers[j]
		if a.FunctionIndex != b.FunctionIndex {
			return a.FunctionIndex < b.FunctionIndex
		}
		if a.SourcePC != b.SourcePC {
			return a.SourcePC < b.SourcePC
		}
		if a.Site != b.Site {
			return a.Site < b.Site
		}
		if a.Identity.Foreign != b.Identity.Foreign {
			// Declared tags first, then the module-external possibility.
			return !a.Identity.Foreign
		}
		return a.Identity.TagIndex < b.Identity.TagIndex
	})

	return flow, nil
}

// PluginBoundaryPolicy is supplied by the caller: the host function imports
// that must be isolated, and the defined functions that form plugin
// boundaries. An isolated import may be given as "module:name" or as a bare
// function name.
type PluginBoundaryPolicy struct {
	IsolatedImports []string
	Boundaries      []int
}

// BoundaryPolicyStatus is the static coverage verdict of a boundary rule.
//
// Policy findings reuse the conservative flow semantics: a call to an isolated
// host import may raise every declared tag or a module-external exception, and
// only catch/catch_ref for that tag identity, or catch_all/catch_all_ref,
// consumes it. No reachability or dynamic alias analysis is added, and a trap
// is not an exception. This is a syntactic policy report, not an observation
// that an exception occurred.
type BoundaryPolicyStatus byte

const (
	// BoundaryCovered means every isolated host-call possibility is fully
	// caught before leaving the boundary.
	BoundaryCovered BoundaryPolicyStatus = iota
	// BoundaryRisk means at least one isolated host-call possibility can
	// escape the boundary.
	BoundaryRisk
	// BoundaryInsufficient means the encoding could not be analyzed. It is
	// never treated as safe.
	BoundaryInsufficient
)

func (s BoundaryPolicyStatus) String() string {
	switch s {
	case BoundaryCovered:
		return "covered"
	case BoundaryRisk:
		return "risk"
	case BoundaryInsufficient:
		return "insufficient"
	default:
		return fmt.Sprintf("status(%d)", byte(s))
	}
}

// PluginHostCallFinding is one possible exception identity of a call to an
// isolated host import inside a boundary function.
type PluginHostCallFinding struct {
	FunctionIndex int
	// SourcePC and SourcePCNext are the call instruction's original span.
	SourcePC     uint64
	SourcePCNext uint64
	ImportModule string
	ImportName   string
	// ImportFunctionIndex is the host function's index in the function
	// index space.
	ImportFunctionIndex int
	Identity            ExceptionIdentity
	// Handler is the catch that matched, or nil if the possibility escapes
	// the boundary.
	Handler *ExceptionHandler
	Target  ExceptionTarget
	Status  BoundaryPolicyStatus
}

// PluginBoundaryResult is the audit of one declared plugin boundary.
type PluginBoundaryResult struct {
	FunctionIndex int
	Status        BoundaryPolicyStatus
	// Findings lists isolated host-call possibilities in stable order.
	Findings []*PluginHostCallFinding
	// EscapingTags and Foreign mirror the boundary's flow summary for
	// context; they are not policy findings by themselves.
	EscapingTags []int
	Foreign      bool
}

// PluginBoundaryAudit is a static boundary policy report. It never reflects
// runtime observation and never claims that an exception or attack occurred.
type PluginBoundaryAudit struct {
	Status              BoundaryPolicyStatus
	InsufficientReasons []string
	IsolatedImports     []string
	Boundaries          []*PluginBoundaryResult
}

// importedFunction is one function import and its function index.
type importedFunction struct {
	FunctionIndex int
	Entry         *ImportEntry
}

func (wf *WasmFile) importedFunctions() []importedFunction {
	fns := make([]importedFunction, 0, len(wf.Import))
	for _, i := range wf.Import {
		if i.Type == types.ExportFunc {
			fns = append(fns, importedFunction{FunctionIndex: len(fns), Entry: i})
		}
	}
	return fns
}

func matchesImportSpec(spec string, e *ImportEntry) bool {
	if idx := strings.IndexByte(spec, ':'); idx >= 0 {
		return spec[:idx] == e.Module && spec[idx+1:] == e.Name
	}
	return spec == e.Name
}

// AuditPluginBoundary performs the static plugin boundary policy check using
// the existing conservative exception-flow semantics. It adds no reachability
// or dynamic alias analysis and treats traps as non-exceptions. When the
// encoding cannot be analyzed, the audit reports insufficient coverage
// instead of defaulting to safe.
func (wf *WasmFile) AuditPluginBoundary(policy *PluginBoundaryPolicy) (*PluginBoundaryAudit, error) {
	if policy == nil {
		return nil, fmt.Errorf("exception flow: nil plugin boundary policy")
	}

	fns := wf.importedFunctions()
	isolated := make(map[int]importedFunction)
	for _, spec := range policy.IsolatedImports {
		matched := false
		for _, fn := range fns {
			if matchesImportSpec(spec, fn.Entry) {
				isolated[fn.FunctionIndex] = fn
				matched = true
			}
		}
		if !matched {
			return nil, fmt.Errorf("exception flow: isolated host import %q is not an imported function", spec)
		}
	}

	prefix := wf.FunctionImportCount()
	boundarySet := make(map[int]struct{})
	for _, b := range policy.Boundaries {
		if b < prefix || b >= prefix+len(wf.Code) {
			return nil, fmt.Errorf("exception flow: boundary function %d is not a defined function", b)
		}
		boundarySet[b] = struct{}{}
	}

	flow, err := wf.BuildExceptionFlow()
	if err != nil {
		return &PluginBoundaryAudit{
			Status:              BoundaryInsufficient,
			InsufficientReasons: []string{err.Error()},
		}, nil
	}

	audit := &PluginBoundaryAudit{Status: BoundaryCovered}
	for _, spec := range policy.IsolatedImports {
		audit.IsolatedImports = append(audit.IsolatedImports, spec)
	}
	sort.Strings(audit.IsolatedImports)

	summaries := make(map[int]*FunctionExceptionSummary, len(flow.Functions))
	for _, s := range flow.Functions {
		summaries[s.FunctionIndex] = s
	}

	boundaries := make([]int, 0, len(boundarySet))
	for b := range boundarySet {
		boundaries = append(boundaries, b)
	}
	sort.Ints(boundaries)

	for _, b := range boundaries {
		result := &PluginBoundaryResult{FunctionIndex: b, Status: BoundaryCovered}
		if s := summaries[b]; s != nil {
			result.EscapingTags = append([]int(nil), s.EscapingTags...)
			result.Foreign = s.Foreign
		}
		for _, t := range flow.Transfers {
			if t.FunctionIndex != b || t.Site != ExceptionSiteCall {
				continue
			}
			fn, ok := isolated[t.Callee]
			if !ok {
				continue
			}
			finding := &PluginHostCallFinding{
				FunctionIndex:       b,
				SourcePC:            t.SourcePC,
				SourcePCNext:        t.SourcePCNext,
				ImportModule:        fn.Entry.Module,
				ImportName:          fn.Entry.Name,
				ImportFunctionIndex: t.Callee,
				Identity:            t.Identity,
				Handler:             t.Handler,
				Target:              t.Target,
				Status:              BoundaryCovered,
			}
			if t.Target.Escapes {
				finding.Status = BoundaryRisk
				result.Status = BoundaryRisk
			}
			result.Findings = append(result.Findings, finding)
		}
		sort.SliceStable(result.Findings, func(i, j int) bool {
			a, bf := result.Findings[i], result.Findings[j]
			if a.SourcePC != bf.SourcePC {
				return a.SourcePC < bf.SourcePC
			}
			if a.Identity.Foreign != bf.Identity.Foreign {
				return !a.Identity.Foreign
			}
			return a.Identity.TagIndex < bf.Identity.TagIndex
		})
		if result.Status == BoundaryRisk && audit.Status == BoundaryCovered {
			audit.Status = BoundaryRisk
		}
		audit.Boundaries = append(audit.Boundaries, result)
	}

	return audit, nil
}
