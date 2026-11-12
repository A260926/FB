// Package analysis performs static, conservative analyses over decoded
// WebAssembly modules without instantiating them.
package analysis

import (
	"bytes"
	"fmt"
	"sort"

	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// EdgeKind says whether a CallEdge was produced by a direct call instruction
// or by expanding a call_indirect against a table summary.
type EdgeKind byte

const (
	// EdgeKindDirect is an edge from an OpcodeCall instruction.
	EdgeKindDirect EdgeKind = iota
	// EdgeKindIndirect is an edge derived from an OpcodeCallIndirect site.
	EdgeKindIndirect
)

// String implements fmt.Stringer.
func (k EdgeKind) String() string {
	switch k {
	case EdgeKindDirect:
		return "direct"
	case EdgeKindIndirect:
		return "indirect"
	default:
		return fmt.Sprintf("unknown(%d)", k)
	}
}

// UnknownReason explains why a table is treated as unknown, i.e. why its
// contents cannot be enumerated from the module alone.
type UnknownReason byte

const (
	// UnknownReasonImportedTable marks a funcref table introduced by an
	// import: its initial contents come from the host.
	UnknownReasonImportedTable UnknownReason = iota
	// UnknownReasonExportedTable marks a funcref table exposed through an
	// export: host code may mutate it after instantiation.
	UnknownReasonExportedTable
	// UnknownReasonActiveOffsetGlobal marks an active element segment whose
	// offset expression is a global.get, so its static placement is unknown.
	UnknownReasonActiveOffsetGlobal
	// UnknownReasonReachableTableSet marks a reachable table.set instruction.
	UnknownReasonReachableTableSet
	// UnknownReasonReachableTableFill marks a reachable table.fill instruction.
	UnknownReasonReachableTableFill
	// UnknownReasonReachableTableGrow marks a reachable table.grow instruction.
	UnknownReasonReachableTableGrow
	// UnknownReasonCopied marks a table that received the unknown state of
	// another table through table.copy.
	UnknownReasonCopied
)

// String implements fmt.Stringer.
func (r UnknownReason) String() string {
	switch r {
	case UnknownReasonImportedTable:
		return "imported funcref table"
	case UnknownReasonExportedTable:
		return "exported funcref table"
	case UnknownReasonActiveOffsetGlobal:
		return "active element segment with global.get offset"
	case UnknownReasonReachableTableSet:
		return "reachable table.set"
	case UnknownReasonReachableTableFill:
		return "reachable table.fill"
	case UnknownReasonReachableTableGrow:
		return "reachable table.grow"
	case UnknownReasonCopied:
		return "table.copy from an unknown table"
	default:
		return fmt.Sprintf("unknown reason(%d)", r)
	}
}

// TableOriginKind identifies the structural source of a table summary entry.
type TableOriginKind byte

const (
	// TableOriginActiveElement is a non-null slot written by an active element
	// segment during module initialization.
	TableOriginActiveElement TableOriginKind = iota
	// TableOriginTableInit is a reference added by a reachable table.init.
	TableOriginTableInit
	// TableOriginTableCopy is a candidate propagated by a reachable table.copy.
	TableOriginTableCopy
)

// String implements fmt.Stringer.
func (k TableOriginKind) String() string {
	switch k {
	case TableOriginActiveElement:
		return "active element segment"
	case TableOriginTableInit:
		return "reachable table.init"
	case TableOriginTableCopy:
		return "reachable table.copy"
	default:
		return fmt.Sprintf("unknown origin(%d)", k)
	}
}

// TableCandidateOrigin traces a single function reference inside a table
// summary back to its source. Origins are deliberately structural rather than
// per-slot: after initialization the summaries are monotone unions, so slot
// identities are no longer maintained.
type TableCandidateOrigin struct {
	// Kind is the source category.
	Kind TableOriginKind
	// SegmentIndex is the element segment index for TableOriginActiveElement
	// and TableOriginTableInit; -1 otherwise.
	SegmentIndex int
	// Function is the full function index containing the instruction, for
	// TableOriginTableInit and TableOriginTableCopy; zero otherwise.
	Function wasm.Index
	// BodyOffset is the instruction offset inside Function's body for
	// TableOriginTableInit and TableOriginTableCopy; zero otherwise.
	BodyOffset uint32
}

// TableUnknownOrigin records one reason a table became unknown. A table that
// is unknown may contain functions outside the module, in addition to every
// candidate still enumerated in its summary.
type TableUnknownOrigin struct {
	// Reason is why the table is unknown.
	Reason UnknownReason
	// SegmentIndex is the active element segment index for
	// UnknownReasonActiveOffsetGlobal; -1 otherwise.
	SegmentIndex int
	// Function is the full function index containing the writing instruction,
	// for reachable table.set/table.fill/table.grow and copied unknown state;
	// zero otherwise.
	Function wasm.Index
	// BodyOffset is the instruction offset inside Function for instruction-
	// level reasons; zero otherwise.
	BodyOffset uint32
	// SourceTable is the table copied from for UnknownReasonCopied; zero
	// otherwise.
	SourceTable wasm.Index
}

// CallEdge is one entry of the conservative call graph.
type CallEdge struct {
	// Caller is the full function index containing the call site.
	Caller wasm.Index
	// CallSiteOffset is the byte offset of the call instruction inside the
	// caller's wasm.Code.Body.
	CallSiteOffset uint32
	// Target is the full function index the call may resolve to. It may refer
	// to an imported function.
	Target wasm.Index
	// Kind distinguishes direct calls from expanded indirect calls.
	Kind EdgeKind
	// Table is the table index consulted by an indirect call; zero for direct
	// edges.
	Table wasm.Index
	// MayCallExternal is true when the edge's call_indirect goes through an
	// unknown table that may also hold functions outside this module.
	MayCallExternal bool
}

// Reachability is the read-only result of BuildReachability. Slices returned
// by its methods are independent copies: mutating them never changes the
// analysis result or the input module.
type Reachability struct {
	roots          []wasm.Index
	rootSet        map[wasm.Index]struct{}
	reachable      map[wasm.Index]struct{}
	edges          []CallEdge
	preds          map[wasm.Index][]edgeParent
	tables         []*tableSummary
	tableOrigins   [][]TableCandidateOrigin
	unknownOrigins [][]TableUnknownOrigin
}

// edgeParent is one incoming edge used to reconstruct root-to-node paths.
type edgeParent struct {
	caller wasm.Index
	offset uint32
	kind   EdgeKind
	table  wasm.Index
}

// siteKey identifies an indirect call site independently of its type use.
type siteKey struct {
	caller wasm.Index
	offset uint32
	table  wasm.Index
}

// Roots returns a copy of the root function indices passed to
// BuildReachability, in the given order.
func (r *Reachability) Roots() []wasm.Index {
	return append([]wasm.Index(nil), r.roots...)
}

// ReachableFunctions returns the indices of every function reachable from the
// roots (the roots themselves included), sorted ascending.
func (r *Reachability) ReachableFunctions() []wasm.Index {
	out := make([]wasm.Index, 0, len(r.reachable))
	for fn := range r.reachable {
		out = append(out, fn)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// IsReachable reports whether the given full function index is reachable.
func (r *Reachability) IsReachable(fn wasm.Index) bool {
	_, ok := r.reachable[fn]
	return ok
}

// Edges returns a copy of the call graph edges, stably sorted by caller
// function index, call site byte offset and target function index.
func (r *Reachability) Edges() []CallEdge {
	out := append([]CallEdge(nil), r.edges...)
	sort.SliceStable(out, func(i, j int) bool {
		if out[i].Caller != out[j].Caller {
			return out[i].Caller < out[j].Caller
		}
		if out[i].CallSiteOffset != out[j].CallSiteOffset {
			return out[i].CallSiteOffset < out[j].CallSiteOffset
		}
		return out[i].Target < out[j].Target
	})
	return out
}

// PathStep is one hop of a PathTo result.
type PathStep struct {
	// Function is the function entered at this hop.
	Function wasm.Index
	// CallSiteOffset is the offset of the call instruction in the previous
	// step's function that reaches this one; zero for the root step.
	CallSiteOffset uint32
	// Kind is the kind of the call edge used for this hop; zero for the root.
	Kind EdgeKind
	// Table is the consulted table for an indirect hop; zero otherwise.
	Table wasm.Index
}

// PathTo returns a call path from one of the roots to the given reachable
// function. The first step is always a root and each following step records
// the call instruction (with its body offset) that leads to the next
// function. It returns nil for an unreachable function.
func (r *Reachability) PathTo(fn wasm.Index) []PathStep {
	if _, ok := r.reachable[fn]; !ok {
		return nil
	}
	var reversed []PathStep
	for {
		if _, isRoot := r.rootSet[fn]; isRoot {
			break // do not walk past a root even if it is itself a call target
		}
		parents := r.preds[fn]
		if len(parents) == 0 {
			break
		}
		p := parents[0]
		reversed = append(reversed, PathStep{
			Function:       fn,
			CallSiteOffset: p.offset,
			Kind:           p.kind,
			Table:          p.table,
		})
		fn = p.caller
	}
	reversed = append(reversed, PathStep{Function: fn})

	out := make([]PathStep, 0, len(reversed))
	for i := len(reversed) - 1; i >= 0; i-- {
		out = append(out, reversed[i])
	}
	return out
}

// TableCandidates returns the module-internal function references statically
// enumerated for the given table, sorted ascending. Unknown tables may hold
// additional, possibly external, functions; use IsTableUnknown for that.
func (r *Reachability) TableCandidates(table wasm.Index) []wasm.Index {
	if int(table) >= len(r.tables) {
		return nil
	}
	return r.tables[table].sortedCandidates()
}

// IsTableUnknown reports whether the table may contain references that cannot
// be enumerated from the module, including functions outside it.
func (r *Reachability) IsTableUnknown(table wasm.Index) bool {
	if int(table) >= len(r.tables) {
		return false
	}
	return r.tables[table].unknown
}

// TableCandidateOrigins returns the traceable sources of every candidate
// enumerated for the given table. The result is a fresh slice.
func (r *Reachability) TableCandidateOrigins(table wasm.Index) []TableCandidateOrigin {
	if int(table) >= len(r.tableOrigins) {
		return nil
	}
	return append([]TableCandidateOrigin(nil), r.tableOrigins[table]...)
}

// TableUnknownOrigins returns the recorded reasons the given table became
// unknown, in discovery order. The result is a fresh slice.
func (r *Reachability) TableUnknownOrigins(table wasm.Index) []TableUnknownOrigin {
	if int(table) >= len(r.unknownOrigins) {
		return nil
	}
	return append([]TableUnknownOrigin(nil), r.unknownOrigins[table]...)
}

// tableSummary is the monotone, path-insensitive summary of one table.
type tableSummary struct {
	candidates map[wasm.Index]struct{}
	unknown    bool
}

func newTableSummary() *tableSummary {
	return &tableSummary{candidates: map[wasm.Index]struct{}{}}
}

func (t *tableSummary) add(fn wasm.Index) (changed bool) {
	if _, ok := t.candidates[fn]; ok {
		return false
	}
	t.candidates[fn] = struct{}{}
	return true
}

func (t *tableSummary) addAll(other *tableSummary) (changed bool) {
	for fn := range other.candidates {
		if t.add(fn) {
			changed = true
		}
	}
	return
}

func (t *tableSummary) sortedCandidates() []wasm.Index {
	out := make([]wasm.Index, 0, len(t.candidates))
	for fn := range t.candidates {
		out = append(out, fn)
	}
	sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
	return out
}

// slotWrite remembers the active segment that last wrote a table slot while
// slot identity is still meaningful.
type slotWrite struct {
	fn      wasm.Index
	segment int
}

// BuildReachability computes a conservative call graph for m starting from
// roots. It never instantiates the module, executes a function or invokes a
// host import.
//
// The roots use the full function index namespace: imported functions come
// first, followed by the functions defined in the module. Direct calls add an
// edge to their target. call_indirect sites are expanded against summaries of
// the referenced tables, matching any function whose parameter and result
// types are item-wise equal to the call's type use, even when the type indices
// differ. ref.func and element declarations are not calls.
//
// Table summaries start from active element segments applied in module order
// (later writes overwrite earlier ones, sparse storage only), then grow
// monotonically as reachable functions perform table.set, table.fill,
// table.grow, table.init and table.copy. The analysis iterates until the
// reachable set, table summaries and call graph stop changing.
//
// Malformed bodies (truncated immediates, out-of-range indices, unsupported
// SIMD/threads/exception-handling encodings) produce an error carrying the
// function index and the body byte offset of the offending instruction.
func BuildReachability(m *wasm.Module, roots []wasm.Index) (*Reachability, error) {
	if m == nil {
		return nil, fmt.Errorf("analysis: nil module")
	}

	importFuncCount := m.ImportFuncCount()
	funcCount := importFuncCount + m.SectionElementCount(wasm.SectionIDFunction)
	tableCount := m.ImportTableCount() + m.SectionElementCount(wasm.SectionIDTable)

	b := &builder{
		m:                 m,
		importFuncCount:   importFuncCount,
		funcCount:         funcCount,
		tableCount:        tableCount,
		tables:            make([]*tableSummary, tableCount),
		tableOrigins:      make([][]TableCandidateOrigin, tableCount),
		unknownOrigins:    make([][]TableUnknownOrigin, tableCount),
		reachable:         map[wasm.Index]struct{}{},
		preds:             map[wasm.Index][]edgeParent{},
		edgeIndex:         map[edgeIdentity]int{},
		externalSites:     map[siteKey]struct{}{},
		scanned:           make(map[wasm.Index]*scannedBody),
		processed:         make(map[wasm.Index]struct{}),
		siteSeen:          map[indirectSite]struct{}{},
		candOriginSeen:    map[int]map[TableCandidateOrigin]struct{}{},
		unknownOriginSeen: map[int]map[TableUnknownOrigin]struct{}{},
	}
	for i := range b.tables {
		b.tables[i] = newTableSummary()
	}

	for _, root := range roots {
		if root >= funcCount {
			return nil, fmt.Errorf("analysis: root function index %d is out of range (module has %d functions)", root, funcCount)
		}
	}

	if err := b.initializeTables(); err != nil {
		return nil, err
	}
	if err := b.prepareSignatureData(); err != nil {
		return nil, err
	}

	b.roots = append([]wasm.Index(nil), roots...)
	queue := append([]wasm.Index(nil), roots...)
	for _, root := range roots {
		b.reachable[root] = struct{}{}
	}

	for len(queue) > 0 {
		fn := queue[0]
		queue = queue[1:]
		if _, ok := b.processed[fn]; ok {
			continue
		}
		b.processed[fn] = struct{}{}

		body, ok, err := b.scan(fn)
		if err != nil {
			return nil, err
		}
		if !ok {
			continue // imported function: no body to scan
		}

		for _, call := range body.direct {
			added, err := b.addDirectEdge(fn, call)
			if err != nil {
				return nil, err
			}
			if added {
				queue = append(queue, call.target)
			}
		}

		for _, eff := range body.effects {
			changed, err := b.applyEffect(fn, eff)
			if err != nil {
				return nil, err
			}
			// Replay durable copy relations: a source table may grow (or
			// become unknown) after its table.copy was first processed.
			if b.replayCopies() {
				changed = true
			}
			if !changed {
				continue
			}
			// Re-expand every reachable indirect site against the new table
			// state, including sites in functions processed earlier. Edge
			// dedup keeps this idempotent.
			for _, site := range b.allSites {
				if _, reachable := b.reachable[site.caller]; !reachable {
					continue
				}
				added, err := b.expandSite(site)
				if err != nil {
					return nil, err
				}
				queue = append(queue, added...)
			}
		}

		// Indirect sites of a newly processed function are expanded against
		// the current summaries; later table changes re-expand every site
		// after the effect handling above.
		for _, site := range body.sites {
			added, err := b.expandSite(site)
			if err != nil {
				return nil, err
			}
			queue = append(queue, added...)
		}
	}

	return b.result(), nil
}

type builder struct {
	m               *wasm.Module
	importFuncCount uint32
	funcCount       uint32
	tableCount      uint32
	roots           []wasm.Index

	tables         []*tableSummary
	tableOrigins   [][]TableCandidateOrigin
	unknownOrigins [][]TableUnknownOrigin

	reachable map[wasm.Index]struct{}
	processed map[wasm.Index]struct{}
	scanned   map[wasm.Index]*scannedBody

	// allSites holds every indirect site discovered in reachable bodies.
	allSites []indirectSite
	// siteSeen deduplicates allSites entries.
	siteSeen map[indirectSite]struct{}

	edges         []CallEdge
	edgeIndex     map[edgeIdentity]int
	preds         map[wasm.Index][]edgeParent
	externalSites map[siteKey]struct{}

	// funcTypeIdx maps every full function index onto its type section index.
	funcTypeIdx []wasm.Index
	// signatureGroups maps a canonical signature to all full function indices
	// having exactly that parameter/result shape.
	signatureGroups map[string][]wasm.Index

	// copyRels records every reachable table.copy as a durable, path-
	// insensitive data-flow relation: a later change to a source table must
	// also flow into the destination even if the copy was processed first.
	copyRels []copyRelation
	// candOriginSeen and unknownOriginSeen keep the per-table origin traces
	// free of duplicate entries while the monotone unions keep growing.
	candOriginSeen    map[int]map[TableCandidateOrigin]struct{}
	unknownOriginSeen map[int]map[TableUnknownOrigin]struct{}
}

// copyRelation is one reachable table.copy instruction.
type copyRelation struct {
	dst    wasm.Index
	src    wasm.Index
	fn     wasm.Index
	offset uint32
}

// scan decodes (once) the body of a defined function. The second result is
// false for imported functions, which have no body.
func (b *builder) scan(fn wasm.Index) (*scannedBody, bool, error) {
	if body, ok := b.scanned[fn]; ok {
		return body, body != nil, nil
	}
	if fn < b.importFuncCount {
		b.scanned[fn] = nil
		return nil, false, nil
	}
	local := fn - b.importFuncCount
	if int(local) >= len(b.m.CodeSection) {
		return nil, false, fmt.Errorf("analysis: function %d has no code body", fn)
	}
	body, err := scanBody(fn, b.m.CodeSection[local].Body)
	if err != nil {
		return nil, false, err
	}
	b.scanned[fn] = body
	return body, true, nil
}

// initializeTables applies the static table contents. Imported or exported
// funcref tables start unknown. Active element segments are then applied in
// module order onto sparse slot maps so that later writes overwrite earlier
// ones and explicit nulls erase previous values; only surviving non-null slots
// become candidates.
func (b *builder) initializeTables() error {
	for _, imp := range b.m.ImportSection {
		if imp.Type == wasm.ExternTypeTable && imp.DescTable != nil &&
			imp.DescTable.Type == wasm.RefTypeFuncref {
			table := b.tableIndexOfImport(imp)
			b.markUnknown(table, TableUnknownOrigin{
				Reason:       UnknownReasonImportedTable,
				SegmentIndex: -1,
			})
		}
	}

	for _, exp := range b.m.ExportSection {
		if exp.Type == wasm.ExternTypeTable && exp.Index < b.tableCount {
			if b.tableRefType(exp.Index) == wasm.RefTypeFuncref {
				b.markUnknown(exp.Index, TableUnknownOrigin{
					Reason:       UnknownReasonExportedTable,
					SegmentIndex: -1,
				})
			}
		}
	}

	// Sparse slot maps keep initialization slot-precise even for tables with
	// a huge Min: capacity is never allocated.
	slots := make([]map[uint64]slotWrite, b.tableCount)
	for i := range slots {
		slots[i] = map[uint64]slotWrite{}
	}

	for segIdx, seg := range b.m.ElementSection {
		if seg == nil || seg.Mode != wasm.ElementModeActive {
			continue // passive and declarative segments never enter tables here
		}
		if seg.Type != wasm.RefTypeFuncref {
			continue
		}
		if seg.TableIndex >= b.tableCount {
			return fmt.Errorf("analysis: element segment %d targets missing table %d", segIdx, seg.TableIndex)
		}
		table := seg.TableIndex

		offset, known, err := b.evalI32Offset(seg.OffsetExpr)
		if err != nil {
			return fmt.Errorf("analysis: element segment %d: %w", segIdx, err)
		}
		if !known {
			b.markUnknown(table, TableUnknownOrigin{
				Reason:       UnknownReasonActiveOffsetGlobal,
				SegmentIndex: segIdx,
			})
			continue
		}

		limit := b.tableMin(table)
		for i, ref := range seg.Init {
			slot := uint64(int64(offset)) + uint64(i) // negative offsets wrap past the limit below
			if slot >= uint64(limit) {
				return fmt.Errorf("analysis: element segment %d writes table %d slot %d which is out of bounds (min %d)",
					segIdx, table, slot, limit)
			}
			if ref == nil {
				delete(slots[table], slot) // null overwrites any earlier write
				continue
			}
			if *ref >= b.funcCount {
				return fmt.Errorf("analysis: element segment %d references missing function %d", segIdx, *ref)
			}
			slots[table][slot] = slotWrite{fn: *ref, segment: segIdx}
		}
	}

	// Freeze the initialization phase into monotone candidate summaries.
	for table, slotMap := range slots {
		for _, write := range slotMap {
			if b.tables[table].add(write.fn) {
				b.addCandidateOrigin(wasm.Index(table), TableCandidateOrigin{
					Kind:         TableOriginActiveElement,
					SegmentIndex: write.segment,
				})
			}
		}
	}
	return nil
}

// addCandidateOrigin appends origin once per table, even if the same source
// contributes candidates repeatedly during the fixed-point iteration.
func (b *builder) addCandidateOrigin(table wasm.Index, origin TableCandidateOrigin) {
	seen, ok := b.candOriginSeen[int(table)]
	if !ok {
		seen = map[TableCandidateOrigin]struct{}{}
		b.candOriginSeen[int(table)] = seen
	}
	if _, ok := seen[origin]; ok {
		return
	}
	seen[origin] = struct{}{}
	b.tableOrigins[table] = append(b.tableOrigins[table], origin)
}

// addUnknownOrigin records an unknown reason once per table.
func (b *builder) addUnknownOrigin(table wasm.Index, origin TableUnknownOrigin) {
	seen, ok := b.unknownOriginSeen[int(table)]
	if !ok {
		seen = map[TableUnknownOrigin]struct{}{}
		b.unknownOriginSeen[int(table)] = seen
	}
	if _, ok := seen[origin]; ok {
		return
	}
	seen[origin] = struct{}{}
	b.unknownOrigins[table] = append(b.unknownOrigins[table], origin)
}

// tableIndexOfImport returns the table-space index occupied by an import.
func (b *builder) tableIndexOfImport(target *wasm.Import) wasm.Index {
	var idx wasm.Index
	for _, imp := range b.m.ImportSection {
		if imp == target {
			return idx
		}
		if imp.Type == wasm.ExternTypeTable {
			idx++
		}
	}
	return idx
}

func (b *builder) tableRefType(table wasm.Index) wasm.RefType {
	if imported := b.importedTable(table); imported != nil {
		return imported.Type
	}
	local := table - b.m.ImportTableCount()
	if int(local) < len(b.m.TableSection) {
		return b.m.TableSection[local].Type
	}
	return 0
}

func (b *builder) tableMin(table wasm.Index) uint32 {
	if imported := b.importedTable(table); imported != nil {
		return imported.Min
	}
	local := table - b.m.ImportTableCount()
	if int(local) < len(b.m.TableSection) {
		return b.m.TableSection[local].Min
	}
	return 0
}

func (b *builder) importedTable(table wasm.Index) *wasm.Table {
	var idx wasm.Index
	for _, imp := range b.m.ImportSection {
		if imp.Type != wasm.ExternTypeTable {
			continue
		}
		if idx == table {
			return imp.DescTable
		}
		idx++
	}
	return nil
}

// evalI32Offset evaluates an active segment offset expression statically. The
// second result is false when the offset comes from a global.get, which makes
// the segment placement unknown.
func (b *builder) evalI32Offset(expr *wasm.ConstantExpression) (int32, bool, error) {
	if expr == nil {
		return 0, false, fmt.Errorf("missing offset expression")
	}
	switch expr.Opcode {
	case wasm.OpcodeI32Const:
		v, _, err := leb128.DecodeInt32(bytes.NewReader(expr.Data))
		if err != nil {
			return 0, false, fmt.Errorf("decode i32.const offset: %w", err)
		}
		return v, true, nil
	case wasm.OpcodeGlobalGet:
		return 0, false, nil
	default:
		return 0, false, fmt.Errorf("unsupported offset opcode %s", wasm.InstructionName(expr.Opcode))
	}
}

// prepareSignatureData builds the full-index type map and the signature
// groups used for structural call_indirect matching.
func (b *builder) prepareSignatureData() error {
	b.funcTypeIdx = make([]wasm.Index, b.funcCount)
	b.signatureGroups = map[string][]wasm.Index{}

	var fn wasm.Index
	for _, imp := range b.m.ImportSection {
		if imp.Type != wasm.ExternTypeFunc {
			continue
		}
		if imp.DescFunc >= uint32(len(b.m.TypeSection)) {
			return fmt.Errorf("analysis: imported function %d references missing type %d", fn, imp.DescFunc)
		}
		b.funcTypeIdx[fn] = imp.DescFunc
		key := canonicalSignature(b.m.TypeSection[imp.DescFunc])
		b.signatureGroups[key] = append(b.signatureGroups[key], fn)
		fn++
	}
	for _, typeIdx := range b.m.FunctionSection {
		if typeIdx >= uint32(len(b.m.TypeSection)) {
			return fmt.Errorf("analysis: function %d references missing type %d", fn, typeIdx)
		}
		b.funcTypeIdx[fn] = typeIdx
		key := canonicalSignature(b.m.TypeSection[typeIdx])
		b.signatureGroups[key] = append(b.signatureGroups[key], fn)
		fn++
	}
	return nil
}

// canonicalSignature renders a function type as a comparable byte string.
func canonicalSignature(t *wasm.FunctionType) string {
	buf := make([]byte, 0, len(t.Params)+len(t.Results)+2)
	buf = append(buf, byte(len(t.Params)))
	buf = append(buf, t.Params...)
	buf = append(buf, byte(len(t.Results)))
	buf = append(buf, t.Results...)
	return string(buf)
}

func (b *builder) markUnknown(table wasm.Index, origin TableUnknownOrigin) {
	if !b.tables[table].unknown {
		b.tables[table].unknown = true
	}
	b.addUnknownOrigin(table, origin)
}

// addDirectEdge records a static call edge. It returns true when the target
// newly became reachable.
func (b *builder) addDirectEdge(caller wasm.Index, call directCall) (bool, error) {
	if call.target >= b.funcCount {
		return false, &ScanError{
			Function: caller,
			Offset:   call.offset,
			Opcode:   wasm.OpcodeCall,
			Err:      fmt.Errorf("call target %d is out of range (module has %d functions)", call.target, b.funcCount),
		}
	}
	edge := CallEdge{
		Caller:         caller,
		CallSiteOffset: call.offset,
		Target:         call.target,
		Kind:           EdgeKindDirect,
	}
	if !b.addEdge(edge) {
		return false, nil
	}
	return b.markReachable(call.target), nil
}

// edgeIdentity identifies an edge independently of MayCallExternal, which can
// flip from false to true when a table becomes unknown.
type edgeIdentity struct {
	caller wasm.Index
	offset uint32
	target wasm.Index
	kind   EdgeKind
	table  wasm.Index
}

func (b *builder) addEdge(edge CallEdge) bool {
	id := edgeIdentity{
		caller: edge.Caller,
		offset: edge.CallSiteOffset,
		target: edge.Target,
		kind:   edge.Kind,
		table:  edge.Table,
	}
	if idx, ok := b.edgeIndex[id]; ok {
		// Upgrade an existing edge in place when its table later became
		// unknown; never keep two edges for the same call target.
		if edge.MayCallExternal && !b.edges[idx].MayCallExternal {
			b.edges[idx].MayCallExternal = true
		}
		return false
	}
	b.edgeIndex[id] = len(b.edges)
	b.edges = append(b.edges, edge)
	b.preds[edge.Target] = append(b.preds[edge.Target], edgeParent{
		caller: edge.Caller,
		offset: edge.CallSiteOffset,
		kind:   edge.Kind,
		table:  edge.Table,
	})
	return true
}

func (b *builder) markReachable(fn wasm.Index) bool {
	if _, ok := b.reachable[fn]; ok {
		return false
	}
	b.reachable[fn] = struct{}{}
	return true
}

func (b *builder) markSiteExternal(key siteKey) {
	b.externalSites[key] = struct{}{}
}

func (b *builder) isSiteExternal(key siteKey) bool {
	_, ok := b.externalSites[key]
	return ok
}

// applyEffect folds one reachable table-modifying instruction into the
// monotone table summaries.
func (b *builder) applyEffect(caller wasm.Index, eff tableEffect) (bool, error) {
	if eff.table >= b.tableCount {
		return false, &ScanError{
			Function: caller,
			Offset:   eff.offset,
			Opcode:   wasm.OpcodeMiscPrefix,
			Err:      fmt.Errorf("table index %d is out of range (module has %d tables)", eff.table, b.tableCount),
		}
	}
	changed := false

	switch eff.kind {
	case tableEffectSet, tableEffectFill, tableEffectGrow:
		// The written value is not tracked per slot in the runtime summary:
		// the table may now hold any funcref, so it becomes unknown. The
		// previously enumerated candidates are deliberately retained.
		reason := UnknownReasonReachableTableSet
		switch eff.kind {
		case tableEffectFill:
			reason = UnknownReasonReachableTableFill
		case tableEffectGrow:
			reason = UnknownReasonReachableTableGrow
		}
		before := b.tables[eff.table].unknown
		b.markUnknown(eff.table, TableUnknownOrigin{
			Reason:       reason,
			SegmentIndex: -1,
			Function:     caller,
			BodyOffset:   eff.offset,
		})
		changed = !before
	case tableEffectInit:
		if eff.segment < 0 || eff.segment >= len(b.m.ElementSection) {
			return false, &ScanError{
				Function: caller,
				Offset:   eff.offset,
				Opcode:   wasm.OpcodeMiscPrefix,
				Err:      fmt.Errorf("table.init element index %d is out of range", eff.segment),
			}
		}
		seg := b.m.ElementSection[eff.segment]
		for _, ref := range seg.Init {
			if ref == nil {
				continue
			}
			if *ref >= b.funcCount {
				return false, &ScanError{
					Function: caller,
					Offset:   eff.offset,
					Opcode:   wasm.OpcodeMiscPrefix,
					Err:      fmt.Errorf("table.init references missing function %d", *ref),
				}
			}
			if b.tables[eff.table].add(*ref) {
				changed = true
			}
			b.addCandidateOrigin(eff.table, TableCandidateOrigin{
				Kind:         TableOriginTableInit,
				SegmentIndex: eff.segment,
				Function:     caller,
				BodyOffset:   eff.offset,
			})
		}
	case tableEffectCopy:
		if eff.srcTable >= b.tableCount {
			return false, &ScanError{
				Function: caller,
				Offset:   eff.offset,
				Opcode:   wasm.OpcodeMiscPrefix,
				Err:      fmt.Errorf("table.copy source index %d is out of range", eff.srcTable),
			}
		}
		// Register the relation durably, then replay every copy relation so
		// that transitive chains and earlier copies pick up current state.
		b.copyRels = append(b.copyRels, copyRelation{
			dst: eff.table, src: eff.srcTable, fn: caller, offset: eff.offset,
		})
		changed = b.replayCopies()
	}
	return changed, nil
}

// replayCopies re-applies every reachable table.copy relation against the
// current summaries. It is run after any table state change because a source
// table can grow after the copy instruction was first processed. Iteration
// handles transitive copy chains and converges since every state transition is
// a monotone union or an unknown flip.
func (b *builder) replayCopies() bool {
	changed := false
	for progress := true; progress; {
		progress = false
		for _, rel := range b.copyRels {
			src, dst := b.tables[rel.src], b.tables[rel.dst]
			if dst.addAll(src) {
				progress = true
				changed = true
				b.addCandidateOrigin(rel.dst, TableCandidateOrigin{
					Kind:       TableOriginTableCopy,
					Function:   rel.fn,
					BodyOffset: rel.offset,
				})
			}
			if src.unknown && !dst.unknown {
				progress = true
				changed = true
				b.markUnknown(rel.dst, TableUnknownOrigin{
					Reason:       UnknownReasonCopied,
					SegmentIndex: -1,
					Function:     rel.fn,
					BodyOffset:   rel.offset,
					SourceTable:  rel.src,
				})
			}
		}
	}
	return changed
}

// expandSite adds the edges from one call_indirect site under the current
// table summaries and returns the newly reachable target functions.
func (b *builder) expandSite(site indirectSite) ([]wasm.Index, error) {
	if _, ok := b.siteSeen[site]; !ok {
		b.siteSeen[site] = struct{}{}
		b.allSites = append(b.allSites, site)
	}
	if site.table >= b.tableCount {
		return nil, &ScanError{
			Function: site.caller,
			Offset:   site.offset,
			Opcode:   wasm.OpcodeCallIndirect,
			Err:      fmt.Errorf("call_indirect table index %d is out of range (module has %d tables)", site.table, b.tableCount),
		}
	}
	if site.typeIdx >= uint32(len(b.m.TypeSection)) {
		return nil, &ScanError{
			Function: site.caller,
			Offset:   site.offset,
			Opcode:   wasm.OpcodeCallIndirect,
			Err:      fmt.Errorf("call_indirect type index %d is out of range", site.typeIdx),
		}
	}

	summary := b.tables[site.table]
	unknown := summary.unknown
	key := siteKey{caller: site.caller, offset: site.offset, table: site.table}
	if unknown {
		b.markSiteExternal(key)
	}

	var added []wasm.Index
	emit := func(target wasm.Index) {
		edge := CallEdge{
			Caller:          site.caller,
			CallSiteOffset:  site.offset,
			Target:          target,
			Kind:            EdgeKindIndirect,
			Table:           site.table,
			MayCallExternal: b.isSiteExternal(key),
		}
		if b.addEdge(edge) && b.markReachable(target) {
			added = append(added, target)
		}
	}

	callType := b.m.TypeSection[site.typeIdx]
	sigKey := canonicalSignature(callType)

	// A concrete table only produces candidates from known non-null slots.
	for fn := range summary.candidates {
		if fn >= b.funcCount {
			continue
		}
		if canonicalSignature(b.m.TypeSection[b.funcTypeIdx[fn]]) == sigKey {
			emit(fn)
		}
	}

	// An unknown table may hold any module-internal function of a compatible
	// signature, plus functions outside the module.
	if unknown {
		for _, fn := range b.signatureGroups[sigKey] {
			emit(fn)
		}
	}
	return added, nil
}

func (b *builder) result() *Reachability {
	rootSet := make(map[wasm.Index]struct{}, len(b.roots))
	for _, root := range b.roots {
		rootSet[root] = struct{}{}
	}

	return &Reachability{
		roots:          b.roots,
		rootSet:        rootSet,
		reachable:      b.reachable,
		edges:          b.edges,
		preds:          b.preds,
		tables:         b.tables,
		tableOrigins:   b.tableOrigins,
		unknownOrigins: b.unknownOrigins,
	}
}
