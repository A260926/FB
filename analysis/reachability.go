// Package analysis implements static analyses over a decoded WebAssembly
// module.
//
// The only entry point currently is BuildReachability, which builds a
// conservative call graph without instantiating the module, executing any
// function, or invoking a host import.
package analysis

import (
	"bytes"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// CallKind distinguishes a statically encoded call from a table-based one.
type CallKind uint8

const (
	// CallDirect is a call instruction naming a function index directly.
	CallDirect CallKind = iota
	// CallIndirect is a call_indirect instruction whose target came from a
	// table.
	CallIndirect
)

// String implements fmt.Stringer.
func (k CallKind) String() string {
	if k == CallIndirect {
		return "indirect"
	}
	return "direct"
}

// Error describes a construct that prevented analysis. For errors found while
// scanning a function body, IsFunction is true and Function/Offset identify
// the function index and byte offset in its Code.Body. For element segment
// errors, Segment is the segment index.
type Error struct {
	// Function is the function index whose body failed to scan. It is only
	// meaningful when IsFunction is true.
	Function wasm.Index
	// IsFunction reports whether Function and Offset are meaningful.
	IsFunction bool
	// Offset is the byte offset from the start of Code.Body where the problem
	// was found. It is only meaningful when IsFunction is true.
	Offset int
	// Segment is the element segment index, or -1 if not applicable.
	Segment int
	// Message explains the problem.
	Message string
}

// Error implements error.
func (e *Error) Error() string {
	if e.IsFunction {
		return fmt.Sprintf("analysis: function %d body offset %d: %s", e.Function, e.Offset, e.Message)
	}
	if e.Segment >= 0 {
		return fmt.Sprintf("analysis: element[%d]: %s", e.Segment, e.Message)
	}
	return "analysis: " + e.Message
}

// FunctionInfo describes a reachable function.
type FunctionInfo struct {
	// Index is the function index in the module's function index space,
	// where function imports precede locally defined functions.
	Index wasm.Index
	// Imported is true when the function is an imported host function.
	Imported bool
	// Path is a call path from a root to this function. The first element is a
	// root and the last is Index. It is a copy owned by the result.
	Path []wasm.Index
}

// CallSite is one conservative call edge: Caller, at Offset in its body, may
// call Target.
type CallSite struct {
	// Caller is the function index containing the call instruction.
	Caller wasm.Index
	// Offset is the byte offset of the call instruction in Caller's Code.Body.
	Offset int
	// Target is the possible callee.
	Target wasm.Index
	// Kind says whether the call was direct or indirect.
	Kind CallKind
	// TableIndex is the table used by an indirect call. It is zero for direct
	// calls and must be interpreted only when Kind is CallIndirect.
	TableIndex wasm.Index
	// TypeIndex is the type index named by call_indirect. It is zero for
	// direct calls and must be interpreted only when Kind is CallIndirect.
	TypeIndex wasm.Index
	// UnknownReason is non-empty when Target came from a table whose contents
	// could not be determined. It explains why the table is unknown.
	UnknownReason string
	// PossibleExternal is true when the table may hold a function defined
	// outside this module.
	PossibleExternal bool
}

// ExpansionKind identifies why a table gained candidates or became unknown.
type ExpansionKind string

const (
	// ExpansionActiveElement is an active element segment applied at
	// instantiation.
	ExpansionActiveElement ExpansionKind = "active-element"
	// ExpansionTableSet is a reachable table.set instruction.
	ExpansionTableSet ExpansionKind = "table.set"
	// ExpansionTableFill is a reachable table.fill instruction.
	ExpansionTableFill ExpansionKind = "table.fill"
	// ExpansionTableGrow is a reachable table.grow instruction.
	ExpansionTableGrow ExpansionKind = "table.grow"
	// ExpansionTableInit is a reachable table.init instruction.
	ExpansionTableInit ExpansionKind = "table.init"
	// ExpansionTableCopy is a reachable table.copy instruction.
	ExpansionTableCopy ExpansionKind = "table.copy"
)

// TableExpansion records how a table's abstract contents were extended.
type TableExpansion struct {
	// Kind identifies the source of the extension.
	Kind ExpansionKind
	// Table is the table whose contents were extended.
	Table wasm.Index
	// SegmentIndex is the element segment for ExpansionActiveElement and
	// ExpansionTableInit, or -1 otherwise.
	SegmentIndex int
	// Writer is the reachable function that performed the runtime write. It is
	// meaningful only when HasWriter is true.
	Writer wasm.Index
	// HasWriter reports whether Writer and Offset belong to a runtime write.
	HasWriter bool
	// Offset is the instruction byte offset in Writer's body when HasWriter is
	// true, or -1 otherwise.
	Offset int
	// SourceTable is the table copied from for ExpansionTableCopy. It is
	// meaningful only when HasSourceTable is true.
	SourceTable wasm.Index
	// HasSourceTable reports whether SourceTable is meaningful.
	HasSourceTable bool
	// Reason is a human-readable explanation of the expansion.
	Reason string
}

// TableInfo summarizes a table in the function index space.
type TableInfo struct {
	// Index is the table index, where imported tables precede locally defined
	// tables.
	Index wasm.Index
	// Unknown is true when the analysis cannot enumerate the table's contents
	// precisely.
	Unknown bool
	// PossibleExternal is true when the table may contain functions from
	// outside this module.
	PossibleExternal bool
	// UnknownReasons are the reasons Unknown was set. They are sorted for
	// stable output.
	UnknownReasons []string
	// Candidates are the definitely known, non-null functions placed in the
	// table by active element segments, table.init, or table.copy. They are
	// sorted by function index. For an unknown table, call_indirect additionally
	// considers every module function with a matching signature.
	Candidates []wasm.Index
	// Expansions trace active element initializations and runtime writes that
	// changed this table. The slice is owned by the result.
	Expansions []TableExpansion
}

// Graph is a conservative call graph. It is a read-only result: all slices are
// owned by the Graph and the input module is not modified.
type Graph struct {
	// Roots are the root function indices, in the order deduplicated.
	Roots []wasm.Index
	// Functions are the reachable functions, sorted by function index.
	Functions []FunctionInfo
	// Calls are the call edges, sorted by caller, call site offset, then
	// target.
	Calls []CallSite
	// Tables describe the tables relevant to reachability, sorted by table
	// index.
	Tables []TableInfo
}

// Reachable reports whether idx is a reachable function in this graph.
func (g *Graph) Reachable(idx wasm.Index) bool {
	for _, f := range g.Functions {
		if f.Index == idx {
			return true
		}
	}
	return false
}

// CallPath returns a copy of a call path from a root to idx, or nil when idx
// is not reachable.
func (g *Graph) CallPath(idx wasm.Index) []wasm.Index {
	for _, f := range g.Functions {
		if f.Index == idx {
			return append([]wasm.Index(nil), f.Path...)
		}
	}
	return nil
}

// BuildReachability computes a conservative call graph rooted at roots.
//
// The analysis does not instantiate the module, execute functions, or invoke
// host imports. roots use the complete function index space: function imports
// come first and only function imports consume function indices. Calls,
// call_indirect targets, active element initialization, and bulk table
// instructions are accumulated until a fixed point is reached.
func BuildReachability(m *wasm.Module, roots []wasm.Index) (*Graph, error) {
	if m == nil {
		return nil, &Error{Segment: -1, Message: "nil module"}
	}

	b := &builder{
		m:         m,
		reachable: map[wasm.Index]struct{}{},
		paths:     map[wasm.Index][]wasm.Index{},
		scans:     map[wasm.Index]*scanResult{},
		siteSeen:  map[siteKey]struct{}{},
		edges:     map[edgeKey]*CallSite{},
	}
	if err := b.initMetadata(); err != nil {
		return nil, err
	}
	if err := b.initTables(); err != nil {
		return nil, err
	}
	if err := b.addRoots(roots); err != nil {
		return nil, err
	}
	if err := b.fixpoint(); err != nil {
		return nil, err
	}
	return b.graph(), nil
}

type functionMeta struct {
	typeIndex  wasm.Index
	imported   bool
	body       []byte
	localCount uint32
}

type edgeKey struct {
	caller wasm.Index
	offset int
	target wasm.Index
}

type siteKey struct {
	caller wasm.Index
	offset int
}

type builder struct {
	m *wasm.Module

	functions []functionMeta
	tables    []*tableState

	funcCount    uint32
	tableCount   uint32
	typeCount    uint32
	globalCount  uint32
	memoryCount  uint32
	dataCount    uint32
	elementCount uint32

	reachable map[wasm.Index]struct{}
	paths     map[wasm.Index][]wasm.Index
	order     []wasm.Index
	roots     []wasm.Index
	scans     map[wasm.Index]*scanResult
	sites     []indirectCall
	siteSeen  map[siteKey]struct{}
	edges     map[edgeKey]*CallSite
}

type tableState struct {
	index      wasm.Index
	refType    wasm.RefType
	min        uint32
	imported   bool
	slots      map[uint32]wasm.Index
	candidates map[wasm.Index]struct{}
	unknown    bool
	external   bool
	reasons    []string

	expansions    []TableExpansion
	expansionKeys map[string]struct{}
}

func (t *tableState) addCandidate(fn wasm.Index) bool {
	if _, ok := t.candidates[fn]; ok {
		return false
	}
	t.candidates[fn] = struct{}{}
	return true
}

func (t *tableState) markUnknown(reason string) bool {
	changed := !t.unknown
	t.unknown = true
	t.external = true
	if reason != "" && !containsString(t.reasons, reason) {
		t.reasons = append(t.reasons, reason)
		changed = true
	}
	return changed
}

func (t *tableState) recordExpansion(e TableExpansion) {
	key := expansionKey(e)
	if _, ok := t.expansionKeys[key]; ok {
		return
	}
	t.expansionKeys[key] = struct{}{}
	t.expansions = append(t.expansions, e)
}

func expansionKey(e TableExpansion) string {
	var sb strings.Builder
	sb.WriteString(string(e.Kind))
	sb.WriteByte('|')
	sb.WriteString(strconv.FormatUint(uint64(e.Table), 10))
	sb.WriteByte('|')
	sb.WriteString(strconv.Itoa(e.SegmentIndex))
	sb.WriteByte('|')
	if e.HasWriter {
		sb.WriteString(strconv.FormatUint(uint64(e.Writer), 10))
	}
	sb.WriteByte('|')
	sb.WriteString(strconv.Itoa(e.Offset))
	sb.WriteByte('|')
	if e.HasSourceTable {
		sb.WriteString(strconv.FormatUint(uint64(e.SourceTable), 10))
	}
	return sb.String()
}

func containsString(haystack []string, needle string) bool {
	for _, s := range haystack {
		if s == needle {
			return true
		}
	}
	return false
}

func (b *builder) initMetadata() *Error {
	m := b.m

	// Function index space: function imports first, then defined functions.
	for i, imp := range m.ImportSection {
		if imp.Type != wasm.ExternTypeFunc {
			continue
		}
		if imp.DescFunc >= uint32(len(m.TypeSection)) {
			return &Error{Segment: -1, Message: fmt.Sprintf("import[%d] function type %d out of range (have %d types)", i, imp.DescFunc, len(m.TypeSection))}
		}
		b.functions = append(b.functions, functionMeta{typeIndex: imp.DescFunc, imported: true})
	}

	if len(m.FunctionSection) != len(m.CodeSection) {
		return &Error{Segment: -1, Message: fmt.Sprintf("function section has %d entries but code section has %d", len(m.FunctionSection), len(m.CodeSection))}
	}
	for i, typeIdx := range m.FunctionSection {
		if typeIdx >= uint32(len(m.TypeSection)) {
			return &Error{Segment: -1, Message: fmt.Sprintf("function[%d] type %d out of range (have %d types)", i, typeIdx, len(m.TypeSection))}
		}
		code := m.CodeSection[i]
		var body []byte
		var locals uint32
		if code != nil {
			body = code.Body
			locals = uint32(len(code.LocalTypes))
		}
		b.functions = append(b.functions, functionMeta{
			typeIndex:  typeIdx,
			body:       body,
			localCount: locals + uint32(len(b.functionType(typeIdx).Params)),
		})
	}
	b.funcCount = uint32(len(b.functions))

	// Other index spaces used to validate immediates.
	for _, imp := range m.ImportSection {
		switch imp.Type {
		case wasm.ExternTypeGlobal:
			b.globalCount++
		case wasm.ExternTypeMemory:
			b.memoryCount++
		}
	}
	b.globalCount += uint32(len(m.GlobalSection))
	if m.MemorySection != nil {
		b.memoryCount++
	}
	b.dataCount = uint32(len(m.DataSection))
	b.elementCount = uint32(len(m.ElementSection))
	b.typeCount = uint32(len(m.TypeSection))
	// Validate every element segment's function references, even when the
	// segment is passive or declarative.
	for si, seg := range m.ElementSection {
		if seg == nil {
			return &Error{Segment: si, Message: "nil element segment"}
		}
		for j, init := range seg.Init {
			if init != nil && *init >= b.funcCount {
				return &Error{Segment: si, Message: fmt.Sprintf("init[%d] function index %d out of range (have %d functions)", j, *init, b.funcCount)}
			}
		}
	}
	return nil
}

func (b *builder) functionType(typeIdx wasm.Index) *wasm.FunctionType {
	if typeIdx >= uint32(len(b.m.TypeSection)) || b.m.TypeSection[typeIdx] == nil {
		return &wasm.FunctionType{}
	}
	return b.m.TypeSection[typeIdx]
}

func (b *builder) functionTypeOf(fn wasm.Index) *wasm.FunctionType {
	return b.functionType(b.functions[fn].typeIndex)
}

// initTables initializes table contents and unknown state. Active element
// segments are applied in module order; later writes overwrite earlier slot
// values, and an unwritten slot is null. Large minima are represented
// sparsely.
func (b *builder) initTables() *Error {
	m := b.m

	// Table index space: imported tables first, then defined tables.
	for _, imp := range m.ImportSection {
		if imp.Type != wasm.ExternTypeTable {
			continue
		}
		var refType wasm.RefType
		var min uint32
		if imp.DescTable != nil {
			refType = imp.DescTable.Type
			min = imp.DescTable.Min
		}
		ts := b.newTableState(refType, min, true)
		if refType == wasm.RefTypeFuncref {
			ts.markUnknown("imported funcref table may contain functions outside the module")
		}
		b.tables = append(b.tables, ts)
	}
	for _, t := range m.TableSection {
		var refType wasm.RefType
		var min uint32
		if t != nil {
			refType = t.Type
			min = t.Min
		}
		b.tables = append(b.tables, b.newTableState(refType, min, false))
	}
	b.tableCount = uint32(len(b.tables))

	// Exported funcref tables are visible to, and mutable by, other modules.
	for _, exp := range m.ExportSection {
		if exp.Type != wasm.ExternTypeTable {
			continue
		}
		if exp.Index >= b.tableCount {
			return &Error{Segment: -1, Message: fmt.Sprintf("export %q table index %d out of range (have %d tables)", exp.Name, exp.Index, b.tableCount)}
		}
		ts := b.tables[exp.Index]
		if ts.refType == wasm.RefTypeFuncref {
			ts.markUnknown("exported funcref table may be modified outside the module")
		}
	}

	for si, seg := range m.ElementSection {
		if seg.Mode != wasm.ElementModeActive {
			continue
		}
		if seg.TableIndex >= b.tableCount {
			return &Error{Segment: si, Message: fmt.Sprintf("active segment table index %d out of range (have %d tables)", seg.TableIndex, b.tableCount)}
		}
		ts := b.tables[seg.TableIndex]
		if seg.Type != wasm.RefTypeFuncref || ts.refType != wasm.RefTypeFuncref {
			// Externref entries are not functions, so they cannot contribute
			// call targets.
			continue
		}

		offset, known, err := elementOffset(seg.OffsetExpr, si)
		if err != nil {
			return err
		}

		ts.recordExpansion(TableExpansion{
			Kind:         ExpansionActiveElement,
			Table:        seg.TableIndex,
			SegmentIndex: si,
			Offset:       -1,
			Reason:       fmt.Sprintf("active element segment [%d] initializes table %d", si, seg.TableIndex),
		})

		if !known {
			ts.markUnknown(fmt.Sprintf("active element segment [%d] has an offset from global.get", si))
			continue
		}
		if offset < 0 {
			return &Error{Segment: si, Message: fmt.Sprintf("active segment offset %d is out of bounds for table %d", offset, seg.TableIndex)}
		}
		if !ts.imported {
			if end := offset + int64(len(seg.Init)); end > int64(ts.min) {
				return &Error{Segment: si, Message: fmt.Sprintf("active segment initializes slots [%d,%d) beyond table %d minimum size %d", offset, end, seg.TableIndex, ts.min)}
			}
		}
		for j, init := range seg.Init {
			slot := uint32(offset) + uint32(j)
			if init == nil {
				delete(ts.slots, slot) // a null entry overwrites a prior function
			} else {
				ts.slots[slot] = *init
			}
		}
	}

	// The initial slot map is the final overwrite state, computed before any
	// runtime write summary; later runtime writes only add candidates.
	for _, ts := range b.tables {
		for _, fn := range ts.slots {
			ts.candidates[fn] = struct{}{}
		}
	}
	return nil
}

func (b *builder) newTableState(refType wasm.RefType, min uint32, imported bool) *tableState {
	return &tableState{
		index:         wasm.Index(len(b.tables)),
		refType:       refType,
		min:           min,
		imported:      imported,
		slots:         map[uint32]wasm.Index{},
		candidates:    map[wasm.Index]struct{}{},
		expansionKeys: map[string]struct{}{},
	}
}

func elementOffset(expr *wasm.ConstantExpression, segment int) (int64, bool, *Error) {
	if expr == nil {
		return 0, false, &Error{Segment: segment, Message: "active element segment has no offset expression"}
	}
	switch expr.Opcode {
	case wasm.OpcodeI32Const:
		v, _, err := leb128.DecodeInt32(bytes.NewReader(expr.Data))
		if err != nil {
			return 0, false, &Error{Segment: segment, Message: "truncated or invalid i32.const offset: " + err.Error()}
		}
		return int64(v), true, nil
	case wasm.OpcodeGlobalGet:
		// The actual global value is unknown without instantiation.
		return 0, false, nil
	default:
		return 0, false, &Error{Segment: segment, Message: fmt.Sprintf("unsupported offset expression opcode 0x%x", expr.Opcode)}
	}
}

func (b *builder) addRoots(roots []wasm.Index) *Error {
	seen := map[wasm.Index]struct{}{}
	for _, root := range roots {
		if root >= b.funcCount {
			return &Error{Segment: -1, Message: fmt.Sprintf("root function index %d out of range (have %d functions)", root, b.funcCount)}
		}
		if _, ok := seen[root]; ok {
			continue
		}
		seen[root] = struct{}{}
		b.roots = append(b.roots, root)
		b.addReachable(root, []wasm.Index{root})
	}
	return nil
}

func (b *builder) addReachable(fn wasm.Index, path []wasm.Index) bool {
	if _, ok := b.reachable[fn]; ok {
		return false
	}
	b.reachable[fn] = struct{}{}
	b.paths[fn] = append([]wasm.Index(nil), path...)
	b.order = append(b.order, fn)
	return true
}

func (b *builder) pathThrough(parent wasm.Index, target wasm.Index) []wasm.Index {
	return append(append([]wasm.Index(nil), b.paths[parent]...), target)
}

func (b *builder) fixpoint() *Error {
	for {
		progress := false

		// Scan every reachable function exactly once.
		for i := 0; i < len(b.order); i++ {
			fn := b.order[i]
			if _, ok := b.scans[fn]; ok {
				continue
			}
			meta := b.functions[fn]
			if meta.imported {
				b.scans[fn] = &scanResult{}
				continue
			}
			res, err := scanFunctionBody(meta.body, fn, b.scanLimits(meta))
			if err != nil {
				return err
			}
			b.scans[fn] = res
			for _, dc := range res.direct {
				if b.addDirectEdge(fn, dc) {
					progress = true
				}
			}
			for _, ic := range res.indirect {
				if b.addIndirectSite(ic) {
					progress = true
				}
			}
		}

		// Apply table writes only from reachable functions. Every application
		// is monotonic, so repeating it is harmless.
		for i := 0; i < len(b.order); i++ {
			fn := b.order[i]
			for _, action := range b.scans[fn].actions {
				if b.applyTableAction(fn, action) {
					progress = true
				}
			}
		}

		// Re-expand every indirect call site, including sites processed in
		// earlier iterations. New candidates can make new functions reachable,
		// and those functions can change tables.
		for _, site := range b.sites {
			if b.expandIndirect(site) {
				progress = true
			}
		}

		if !progress {
			return nil
		}
	}
}

func (b *builder) addDirectEdge(caller wasm.Index, dc directCall) bool {
	key := edgeKey{caller: caller, offset: dc.offset, target: dc.target}
	if _, ok := b.edges[key]; ok {
		return false
	}
	b.edges[key] = &CallSite{
		Caller: caller,
		Offset: dc.offset,
		Target: dc.target,
		Kind:   CallDirect,
	}
	if _, ok := b.reachable[dc.target]; !ok {
		b.addReachable(dc.target, b.pathThrough(caller, dc.target))
		return true
	}
	return false
}

func (b *builder) addIndirectSite(ic indirectCall) bool {
	key := siteKey{caller: ic.caller, offset: ic.offset}
	if _, ok := b.siteSeen[key]; ok {
		return false
	}
	b.siteSeen[key] = struct{}{}
	b.sites = append(b.sites, ic)
	return true
}

func sameSignature(a, b *wasm.FunctionType) bool {
	if len(a.Params) != len(b.Params) || len(a.Results) != len(b.Results) {
		return false
	}
	for i := range a.Params {
		if a.Params[i] != b.Params[i] {
			return false
		}
	}
	for i := range a.Results {
		if a.Results[i] != b.Results[i] {
			return false
		}
	}
	return true
}

func (b *builder) compatibleFunctions(want *wasm.FunctionType) []wasm.Index {
	var out []wasm.Index
	for i := uint32(0); i < b.funcCount; i++ {
		if sameSignature(b.functionTypeOf(i), want) {
			out = append(out, i)
		}
	}
	return out
}

func (b *builder) expandIndirect(site indirectCall) bool {
	ts := b.tables[site.table]
	want := b.functionType(site.typeIdx)

	var targets []wasm.Index
	var reason string
	var external bool
	if ts.unknown {
		targets = b.compatibleFunctions(want)
		reason = strings.Join(ts.reasons, "; ")
		if reason == "" {
			reason = "table contents are unknown"
		}
		external = true
	} else {
		// Iterate concrete candidates in index order so paths are
		// deterministic.
		for fn := range ts.candidates {
			if sameSignature(b.functionTypeOf(fn), want) {
				targets = append(targets, fn)
			}
		}
		sort.Slice(targets, func(i, j int) bool { return targets[i] < targets[j] })
	}

	progress := false
	for _, target := range targets {
		key := edgeKey{caller: site.caller, offset: site.offset, target: target}
		if edge, ok := b.edges[key]; ok {
			// The reason can become known after an earlier iteration.
			edge.Kind = CallIndirect
			edge.TableIndex = site.table
			edge.TypeIndex = site.typeIdx
			edge.UnknownReason = reason
			edge.PossibleExternal = external
			continue
		}
		b.edges[key] = &CallSite{
			Caller:           site.caller,
			Offset:           site.offset,
			Target:           target,
			Kind:             CallIndirect,
			TableIndex:       site.table,
			TypeIndex:        site.typeIdx,
			UnknownReason:    reason,
			PossibleExternal: external,
		}
		if _, ok := b.reachable[target]; !ok {
			b.addReachable(target, b.pathThrough(site.caller, target))
			progress = true
		}
	}
	return progress
}

func (b *builder) applyTableAction(fn wasm.Index, action tableAction) bool {
	changed := false
	switch action.kind {
	case tableActionSet, tableActionFill, tableActionGrow:
		ts := b.tables[action.table]
		reason := fmt.Sprintf("%s in function %d at body offset %d", actionKindName(action.kind), fn, action.offset)
		if ts.markUnknown(reason) {
			changed = true
		}
		ts.recordExpansion(TableExpansion{
			Kind:         actionExpansionKind(action.kind),
			Table:        action.table,
			SegmentIndex: -1,
			Writer:       fn,
			HasWriter:    true,
			Offset:       action.offset,
			Reason:       reason,
		})
	case tableActionInit:
		seg := b.m.ElementSection[action.segment]
		ts := b.tables[action.table]
		if seg != nil && seg.Type == wasm.RefTypeFuncref && ts.refType == wasm.RefTypeFuncref {
			// Path-insensitive: every non-null reference in the segment
			// becomes a possible table entry, whether or not the runtime
			// would actually copy it.
			for _, init := range seg.Init {
				if init != nil && ts.addCandidate(*init) {
					changed = true
				}
			}
		}
		ts.recordExpansion(TableExpansion{
			Kind:         ExpansionTableInit,
			Table:        action.table,
			SegmentIndex: int(action.segment),
			Writer:       fn,
			HasWriter:    true,
			Offset:       action.offset,
			Reason:       fmt.Sprintf("table.init in function %d at body offset %d copies element segment [%d]", fn, action.offset, action.segment),
		})
	case tableActionCopy:
		src, dst := b.tables[action.source], b.tables[action.table]
		for cand := range src.candidates {
			if dst.addCandidate(cand) {
				changed = true
			}
		}
		if src.unknown {
			reason := fmt.Sprintf("table.copy in function %d at body offset %d from unknown table %d", fn, action.offset, action.source)
			if dst.markUnknown(reason) {
				changed = true
			}
			if src.external && !dst.external {
				dst.external = true
				changed = true
			}
		}
		dst.recordExpansion(TableExpansion{
			Kind:           ExpansionTableCopy,
			Table:          action.table,
			SegmentIndex:   -1,
			Writer:         fn,
			HasWriter:      true,
			Offset:         action.offset,
			SourceTable:    action.source,
			HasSourceTable: true,
			Reason:         fmt.Sprintf("table.copy in function %d at body offset %d copies table %d to table %d", fn, action.offset, action.source, action.table),
		})
	case tableActionDrop:
		// elem.drop is intentionally ignored: this analysis is path-insensitive,
		// so a dropped segment still contributes candidates.
	}
	return changed
}

func actionKindName(kind tableActionKind) string {
	switch kind {
	case tableActionSet:
		return "table.set"
	case tableActionFill:
		return "table.fill"
	case tableActionGrow:
		return "table.grow"
	case tableActionInit:
		return "table.init"
	case tableActionCopy:
		return "table.copy"
	case tableActionDrop:
		return "elem.drop"
	}
	return "table write"
}

func actionExpansionKind(kind tableActionKind) ExpansionKind {
	switch kind {
	case tableActionSet:
		return ExpansionTableSet
	case tableActionFill:
		return ExpansionTableFill
	case tableActionGrow:
		return ExpansionTableGrow
	case tableActionInit:
		return ExpansionTableInit
	case tableActionCopy:
		return ExpansionTableCopy
	}
	return ExpansionTableSet
}

func (b *builder) scanLimits(meta functionMeta) scanLimits {
	return scanLimits{
		funcCount:    b.funcCount,
		tableCount:   b.tableCount,
		typeCount:    b.typeCount,
		globalCount:  b.globalCount,
		memoryCount:  b.memoryCount,
		dataCount:    b.dataCount,
		elementCount: b.elementCount,
		localCount:   meta.localCount,
	}
}

func (b *builder) graph() *Graph {
	g := &Graph{Roots: append([]wasm.Index(nil), b.roots...)}

	g.Functions = make([]FunctionInfo, 0, len(b.order))
	for _, fn := range b.order {
		g.Functions = append(g.Functions, FunctionInfo{
			Index:    fn,
			Imported: b.functions[fn].imported,
			Path:     append([]wasm.Index(nil), b.paths[fn]...),
		})
	}
	sort.Slice(g.Functions, func(i, j int) bool {
		return g.Functions[i].Index < g.Functions[j].Index
	})

	g.Calls = make([]CallSite, 0, len(b.edges))
	for _, edge := range b.edges {
		g.Calls = append(g.Calls, *edge)
	}
	sort.Slice(g.Calls, func(i, j int) bool {
		a, c := g.Calls[i], g.Calls[j]
		if a.Caller != c.Caller {
			return a.Caller < c.Caller
		}
		if a.Offset != c.Offset {
			return a.Offset < c.Offset
		}
		return a.Target < c.Target
	})

	g.Tables = make([]TableInfo, 0, len(b.tables))
	for _, ts := range b.tables {
		info := TableInfo{
			Index:            ts.index,
			Unknown:          ts.unknown,
			PossibleExternal: ts.external,
			UnknownReasons:   append([]string(nil), ts.reasons...),
			Candidates:       make([]wasm.Index, 0, len(ts.candidates)),
		}
		for fn := range ts.candidates {
			info.Candidates = append(info.Candidates, fn)
		}
		sort.Slice(info.Candidates, func(i, j int) bool {
			return info.Candidates[i] < info.Candidates[j]
		})
		sort.Strings(info.UnknownReasons)
		info.Expansions = append([]TableExpansion(nil), ts.expansions...)
		g.Tables = append(g.Tables, info)
	}
	sort.Slice(g.Tables, func(i, j int) bool {
		return g.Tables[i].Index < g.Tables[j].Index
	})
	return g
}
