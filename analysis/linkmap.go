// Package analysis provides static cross-module analysis of WebAssembly
// binaries. It resolves imports and re-export aliases across a bundle of
// modules, reconstructs the static initial layout of shared memories and
// tables, and audits which module initialized which parts of them.
//
// The analysis never instantiates the modules: function bodies, start
// functions and host imports are not run. The layouts reported here are the
// active data and funcref element segments of each module stacked in a
// caller-chosen initialization order; they do not represent memory contents
// after the program runs.
package analysis

import (
	"bytes"
	"fmt"
	"sort"
	"strings"

	"github.com/tetratelabs/wabin/binary"
	"github.com/tetratelabs/wabin/leb128"
	"github.com/tetratelabs/wabin/wasm"
)

// ModuleInput is one module of the bundle analyzed by BuildBundle.
type ModuleInput struct {
	// Name is the link name other modules use to import from this module. It must be unique across the bundle.
	Name string
	// Binary is the module in the WebAssembly binary format (%.wasm). It is decoded during BuildBundle and not
	// retained, so mutating it afterwards does not change the analysis result.
	Binary []byte
}

// SymbolRef identifies one entry of a module's index space, e.g. the function at index 3 of module "app".
//
// Note: each external kind (func, table, memory, global) has its own index space per module, beginning with the
// imports of that kind. The position of an import in the import section is not its index.
type SymbolRef struct {
	Module string
	Kind   wasm.ExternType
	Index  uint32
}

// String implements fmt.Stringer, returning e.g. "app.func[3]".
func (s SymbolRef) String() string {
	return fmt.Sprintf("%s.%s[%d]", s.Module, wasm.ExternTypeName(s.Kind), s.Index)
}

// ResolvedSymbol is the outcome of resolving a symbol through import aliases and re-exports.
type ResolvedSymbol struct {
	// Final is the module, kind and index of the actual definition the symbol resolves to. Definitions are
	// never merged: two modules defining same-named exports remain distinct finals.
	Final SymbolRef
	// Chain is the hop-by-hop path taken to reach Final, starting at the queried symbol and ending at Final.
	Chain []SymbolRef
}

// ResourceID identifies a final (defining) memory or table of a bundle.
type ResourceID struct {
	Module string
	Kind   wasm.ExternType // wasm.ExternTypeMemory or wasm.ExternTypeTable
	Index  uint32
}

// String implements fmt.Stringer, returning e.g. "core.memory[0]".
func (r ResourceID) String() string {
	return fmt.Sprintf("%s.%s[%d]", r.Module, wasm.ExternTypeName(r.Kind), r.Index)
}

// StartFunc records the start function of a module. Its effects are not analyzed.
type StartFunc struct {
	Module string
	// Index is the function index in the module's function index space.
	Index uint32
}

// MemoryFragment is one half-open piece [Start, End) of a memory layout query. Fragments are split wherever the
// source of the content changes.
type MemoryFragment struct {
	Start, End uint64
	// Written is false when no data segment ever covered this range: the content is zero and carries no source.
	Written bool
	// Module is the module whose data segment wrote this range. Empty when Written is false.
	Module string
	// Segment is the index of the writing segment in Module's data section. -1 when Written is false.
	Segment int
	// Offset is the position of this fragment inside the writing segment's Init.
	Offset uint64
	// Data is the content of this range. Nil when Written is false, meaning zero: zero holes of any size are
	// reported as a single fragment without expanding them byte by byte.
	Data []byte
}

// TableSlot is the content of one table slot: either a function reference or null.
type TableSlot struct {
	Null bool
	// Function is the final definition of the referenced function, valid when Null is false.
	Function SymbolRef
}

// TableFragment is one half-open piece [Start, End) of a table layout query, in slots. Fragments are split
// wherever the source of the content changes.
type TableFragment struct {
	Start, End uint64
	// Written is false when no element segment ever covered this range: the slots are null and carry no source.
	Written bool
	// Module is the module whose element segment wrote this range. Empty when Written is false.
	Module string
	// Segment is the index of the writing segment in Module's element section. -1 when Written is false.
	Segment int
	// Offset is the position of this fragment inside the writing segment's Init.
	Offset uint64
	// Slots is the content of this range. Nil when Written is false, meaning all null.
	Slots []TableSlot
}

// SourceRef identifies a data or element segment of a module, used as the previous source of an overridden range.
type SourceRef struct {
	Module  string
	Segment int
}

// WriteRecord describes one write of a data or element segment into a final shared resource.
type WriteRecord struct {
	Resource ResourceID
	// Chain is the alias chain from the writer's own reference to the final resource, e.g. an imported memory
	// re-exported by an intermediate module.
	Chain   []SymbolRef
	Writer  string
	Segment int
	// Start and End are the half-open range written: bytes for memories, slots for tables.
	Start, End uint64
}

// OverrideRecord describes a write that covered content an earlier segment had already initialized. The previous
// source is recorded while the segments are stacked, so it is exact even when the old and new bytes are identical.
type OverrideRecord struct {
	WriteRecord
	// Previous is the source that initialized the overridden range before this write.
	Previous SourceRef
}

// AuditPolicy configures Bundle.Audit.
type AuditPolicy struct {
	// Allow maps each final memory or table to the modules permitted to initialize it. A resource missing from
	// Allow has no permitted writers, so every write to it is reported as unauthorized.
	Allow map[ResourceID][]string
}

// AuditReport is the outcome of Bundle.Audit. It only covers initialization: what each module was allowed to
// write and where the written content came from. It says nothing about what start functions or bodies may do
// when they run.
type AuditReport struct {
	// Unauthorized lists each write a module made into a resource it was not permitted to initialize.
	Unauthorized []WriteRecord
	// Overrides lists each write that covered initialization content of a different module, even when the
	// overwritten content happened to be identical.
	Overrides []OverrideRecord
}

// Bundle is the owning, read-only result of BuildBundle. All state is decoded and copied during the build, so
// later mutation of the input binaries does not change it.
type Bundle struct {
	modules   []*moduleNode // in input order
	byName    map[string]*moduleNode
	initOrder []string

	resolved   map[SymbolRef]ResolvedSymbol
	globalVals map[SymbolRef]uint32 // evaluated immutable i32 globals, by final definition

	memories []*memoryResource // in module input order, then index
	tables   []*tableResource
	memByRef map[SymbolRef]*memoryResource // by final definition
	tabByRef map[SymbolRef]*tableResource

	starts []StartFunc
}

// BuildBundle decodes each input with binary.DecodeModule and statically analyzes the relationships between the
// modules. initOrder must list every module exactly once: it decides which module's segments override which when
// they write the same shared memory or table. Within one module, segments apply in their original order.
//
// The inputs are assumed to have passed the core type checks already; this analysis checks the relationships
// between modules instead: every import is resolved to its final definition and validated against it, and the
// active data and funcref element segments are stacked onto the final resources. On any error, e.g. a duplicate
// module name, an unresolvable import or an out of bounds segment, no partial result is returned.
func BuildBundle(inputs []ModuleInput, initOrder []string) (*Bundle, error) {
	b := &Bundle{
		byName:     map[string]*moduleNode{},
		resolved:   map[SymbolRef]ResolvedSymbol{},
		globalVals: map[SymbolRef]uint32{},
		memByRef:   map[SymbolRef]*memoryResource{},
		tabByRef:   map[SymbolRef]*tableResource{},
	}
	for _, in := range inputs {
		if _, dup := b.byName[in.Name]; dup {
			return nil, fmt.Errorf("duplicate module name %q", in.Name)
		}
		mod, err := binary.DecodeModule(in.Binary, wasm.CoreFeaturesV2)
		if err != nil {
			return nil, fmt.Errorf("module %q: %v", in.Name, err)
		}
		node := &moduleNode{name: in.Name, mod: mod}
		node.funcs, node.tables, node.mems, node.globals = indexSpaces(mod)
		b.modules = append(b.modules, node)
		b.byName[in.Name] = node
	}

	if len(initOrder) != len(inputs) {
		return nil, fmt.Errorf("initOrder must list each of the %d modules exactly once, but lists %d names", len(inputs), len(initOrder))
	}
	seen := map[string]bool{}
	for _, n := range initOrder {
		if b.byName[n] == nil {
			return nil, fmt.Errorf("initOrder: unknown module %q", n)
		}
		if seen[n] {
			return nil, fmt.Errorf("initOrder: module %q listed twice", n)
		}
		seen[n] = true
	}
	b.initOrder = append([]string(nil), initOrder...)

	// Export indices must be valid even when nothing imports them.
	for _, node := range b.modules {
		for i, exp := range node.mod.ExportSection {
			space := node.space(exp.Type)
			if space == nil || int(exp.Index) >= len(space) {
				return nil, fmt.Errorf("module %q export[%d] %q: %s index %d out of range",
					node.name, i, exp.Name, wasm.ExternTypeName(exp.Type), exp.Index)
			}
		}
	}

	// Resolve every symbol of every module to its final definition, and validate each import against the type
	// of that definition (not against any intermediate alias declaration).
	for _, node := range b.modules {
		for _, kind := range externKinds {
			space := node.space(kind)
			for idx, e := range space {
				res, err := b.resolve(SymbolRef{node.name, kind, uint32(idx)})
				if err != nil {
					return nil, err
				}
				if e.imported {
					if err := b.checkImport(node, e, res); err != nil {
						return nil, err
					}
				}
			}
		}
	}

	// Collect the final resources: one per memory or table definition. Same-named definitions in different
	// modules are distinct resources.
	for _, node := range b.modules {
		for idx, e := range node.mems {
			if e.imported {
				continue
			}
			id := ResourceID{node.name, wasm.ExternTypeMemory, uint32(idx)}
			r := &memoryResource{id: id, capacity: uint64(node.mod.MemorySection.Min) << wasm.MemoryPageSizeInBits}
			r.ov = newOverlay(r.capacity)
			b.memories = append(b.memories, r)
			b.memByRef[SymbolRef{id.Module, id.Kind, id.Index}] = r
		}
		for idx, e := range node.tables {
			if e.imported {
				continue
			}
			t := node.mod.TableSection[idx-importCount(node.tables)]
			id := ResourceID{node.name, wasm.ExternTypeTable, uint32(idx)}
			r := &tableResource{id: id, capacity: uint64(t.Min)}
			r.ov = newOverlay(r.capacity)
			b.tables = append(b.tables, r)
			b.tabByRef[SymbolRef{id.Module, id.Kind, id.Index}] = r
		}
	}

	// Start functions are recorded, but their effects are not analyzed.
	for _, node := range b.modules {
		if node.mod.StartSection != nil {
			b.starts = append(b.starts, StartFunc{Module: node.name, Index: *node.mod.StartSection})
		}
	}

	if err := b.buildLayouts(); err != nil {
		return nil, err
	}
	return b, nil
}

// Modules returns the module names in input order.
func (b *Bundle) Modules() []string {
	out := make([]string, len(b.modules))
	for i, n := range b.modules {
		out[i] = n.name
	}
	return out
}

// InitOrder returns the initialization order the bundle was built with.
func (b *Bundle) InitOrder() []string {
	return append([]string(nil), b.initOrder...)
}

// Starts returns the start functions of all modules in input order. Their effects are not analyzed.
func (b *Bundle) Starts() []StartFunc {
	return append([]StartFunc(nil), b.starts...)
}

// Resolve follows the import aliases and re-export hops of the given symbol to the definition that finally
// provides it, returning that definition and the hop-by-hop chain.
func (b *Bundle) Resolve(module string, kind wasm.ExternType, index uint32) (ResolvedSymbol, error) {
	node := b.byName[module]
	if node == nil {
		return ResolvedSymbol{}, fmt.Errorf("module %q not found", module)
	}
	space := node.space(kind)
	if space == nil {
		return ResolvedSymbol{}, fmt.Errorf("module %q: unsupported kind %s", module, wasm.ExternTypeName(kind))
	}
	if int(index) >= len(space) {
		return ResolvedSymbol{}, fmt.Errorf("module %q: %s index %d out of range (%d entries)",
			module, wasm.ExternTypeName(kind), index, len(space))
	}
	res := b.resolved[SymbolRef{module, kind, index}]
	res.Chain = append([]SymbolRef(nil), res.Chain...)
	return res, nil
}

// Resources returns the final resources of the given kind (wasm.ExternTypeMemory or wasm.ExternTypeTable) in
// module input order, then index order.
func (b *Bundle) Resources(kind wasm.ExternType) []ResourceID {
	var out []ResourceID
	switch kind {
	case wasm.ExternTypeMemory:
		for _, r := range b.memories {
			out = append(out, r.id)
		}
	case wasm.ExternTypeTable:
		for _, r := range b.tables {
			out = append(out, r.id)
		}
	}
	return out
}

// MemoryCapacity returns the initial size in bytes of the memory the given reference resolves to. The capacity
// comes from the initial Min of the final definition, not from any import declaration along the chain.
func (b *Bundle) MemoryCapacity(module string, index uint32) (uint64, error) {
	r, err := b.memoryResourceFor(module, index)
	if err != nil {
		return 0, err
	}
	return r.capacity, nil
}

// TableCapacity returns the initial size in slots of the table the given reference resolves to. The capacity
// comes from the initial Min of the final definition, not from any import declaration along the chain.
func (b *Bundle) TableCapacity(module string, index uint32) (uint64, error) {
	r, err := b.tableResourceFor(module, index)
	if err != nil {
		return 0, err
	}
	return r.capacity, nil
}

// MemoryLayout returns the half-open fragments covering the byte range [start, end) of the memory the given
// reference resolves to. Querying through any alias of the same final memory returns the same layout.
func (b *Bundle) MemoryLayout(module string, index uint32, start, end uint64) ([]MemoryFragment, error) {
	r, err := b.memoryResourceFor(module, index)
	if err != nil {
		return nil, err
	}
	if start > end || end > r.capacity {
		return nil, fmt.Errorf("module %q memory %d: range [%d, %d) out of capacity %d", module, index, start, end, r.capacity)
	}
	var out []MemoryFragment
	for _, sp := range r.ov.spans {
		lo, hi, ok := clip(sp, start, end)
		if !ok {
			continue
		}
		f := MemoryFragment{Start: lo, End: hi, Segment: -1}
		if sp.write >= 0 {
			w := r.writes[sp.write]
			off := sp.off + (lo - sp.start)
			f.Written = true
			f.Module = w.rec.Writer
			f.Segment = w.rec.Segment
			f.Offset = off
			f.Data = append([]byte(nil), w.data[off:off+hi-lo]...)
		}
		out = append(out, f)
	}
	return out, nil
}

// TableLayout returns the half-open fragments covering the slot range [start, end) of the table the given
// reference resolves to. Querying through any alias of the same final table returns the same layout.
func (b *Bundle) TableLayout(module string, index uint32, start, end uint64) ([]TableFragment, error) {
	r, err := b.tableResourceFor(module, index)
	if err != nil {
		return nil, err
	}
	if start > end || end > r.capacity {
		return nil, fmt.Errorf("module %q table %d: range [%d, %d) out of capacity %d", module, index, start, end, r.capacity)
	}
	var out []TableFragment
	for _, sp := range r.ov.spans {
		lo, hi, ok := clip(sp, start, end)
		if !ok {
			continue
		}
		f := TableFragment{Start: lo, End: hi, Segment: -1}
		if sp.write >= 0 {
			w := r.writes[sp.write]
			off := sp.off + (lo - sp.start)
			f.Written = true
			f.Module = w.rec.Writer
			f.Segment = w.rec.Segment
			f.Offset = off
			f.Slots = append([]TableSlot(nil), w.slots[off:off+hi-lo]...)
		}
		out = append(out, f)
	}
	return out, nil
}

// Audit reports the writes each module was not permitted to make under policy, and the writes that covered
// another module's initialization content. The previous sources were recorded while the segments were stacked,
// so overrides are exact even when old and new content are identical.
func (b *Bundle) Audit(policy AuditPolicy) *AuditReport {
	allowed := func(id ResourceID, writer string) bool {
		for _, m := range policy.Allow[id] {
			if m == writer {
				return true
			}
		}
		return false
	}
	rep := &AuditReport{}
	collect := func(id ResourceID, writes []WriteRecord, overrides []OverrideRecord) {
		for _, rec := range writes {
			if !allowed(id, rec.Writer) {
				rep.Unauthorized = append(rep.Unauthorized, cloneWriteRecord(rec))
			}
		}
		for _, ov := range overrides {
			if ov.Previous.Module != ov.Writer {
				ov.WriteRecord = cloneWriteRecord(ov.WriteRecord)
				rep.Overrides = append(rep.Overrides, ov)
			}
		}
	}
	for _, r := range b.memories {
		writes := make([]WriteRecord, len(r.writes))
		for i, w := range r.writes {
			writes[i] = w.rec
		}
		collect(r.id, writes, r.overrides)
	}
	for _, r := range b.tables {
		writes := make([]WriteRecord, len(r.writes))
		for i, w := range r.writes {
			writes[i] = w.rec
		}
		collect(r.id, writes, r.overrides)
	}
	return rep
}

func cloneWriteRecord(rec WriteRecord) WriteRecord {
	rec.Chain = append([]SymbolRef(nil), rec.Chain...)
	return rec
}

func (b *Bundle) memoryResourceFor(module string, index uint32) (*memoryResource, error) {
	node := b.byName[module]
	if node == nil {
		return nil, fmt.Errorf("module %q not found", module)
	}
	if int(index) >= len(node.mems) {
		return nil, fmt.Errorf("module %q: memory index %d out of range (%d memories)", module, index, len(node.mems))
	}
	return b.memByRef[b.resolved[SymbolRef{module, wasm.ExternTypeMemory, index}].Final], nil
}

func (b *Bundle) tableResourceFor(module string, index uint32) (*tableResource, error) {
	node := b.byName[module]
	if node == nil {
		return nil, fmt.Errorf("module %q not found", module)
	}
	if int(index) >= len(node.tables) {
		return nil, fmt.Errorf("module %q: table index %d out of range (%d tables)", module, index, len(node.tables))
	}
	return b.tabByRef[b.resolved[SymbolRef{module, wasm.ExternTypeTable, index}].Final], nil
}

var externKinds = []wasm.ExternType{wasm.ExternTypeFunc, wasm.ExternTypeTable, wasm.ExternTypeMemory, wasm.ExternTypeGlobal}

// moduleNode is one decoded module with its per-kind index spaces.
type moduleNode struct {
	name    string
	mod     *wasm.Module
	funcs   []symEntry
	tables  []symEntry
	mems    []symEntry
	globals []symEntry
}

// space returns the index space of the given kind, or nil for an unknown kind.
func (n *moduleNode) space(kind wasm.ExternType) []symEntry {
	switch kind {
	case wasm.ExternTypeFunc:
		return n.funcs
	case wasm.ExternTypeTable:
		return n.tables
	case wasm.ExternTypeMemory:
		return n.mems
	case wasm.ExternTypeGlobal:
		return n.globals
	}
	return nil
}

// symEntry is one index space entry: either an import or a local definition.
type symEntry struct {
	imported bool
	imp      *wasm.Import
	impPos   int // position in the import section, only meaningful when imported
}

// indexSpaces splits the import section into per-kind index spaces followed by the module's own definitions.
// The position of an import in the import section is not its index: each kind counts separately.
func indexSpaces(m *wasm.Module) (funcs, tables, mems, globals []symEntry) {
	for pos, imp := range m.ImportSection {
		e := symEntry{imported: true, imp: imp, impPos: pos}
		switch imp.Type {
		case wasm.ExternTypeFunc:
			funcs = append(funcs, e)
		case wasm.ExternTypeTable:
			tables = append(tables, e)
		case wasm.ExternTypeMemory:
			mems = append(mems, e)
		case wasm.ExternTypeGlobal:
			globals = append(globals, e)
		}
	}
	for range m.FunctionSection {
		funcs = append(funcs, symEntry{})
	}
	for range m.TableSection {
		tables = append(tables, symEntry{})
	}
	if m.MemorySection != nil {
		mems = append(mems, symEntry{})
	}
	for range m.GlobalSection {
		globals = append(globals, symEntry{})
	}
	return
}

// importCount returns how many entries of an index space are imports. Definitions follow them.
func importCount(space []symEntry) (n int) {
	for _, e := range space {
		if e.imported {
			n++
		}
	}
	return
}

// resolve follows import aliases and re-exports from ref to its final definition, caching every hop it walks.
func (b *Bundle) resolve(ref SymbolRef) (ResolvedSymbol, error) {
	if r, ok := b.resolved[ref]; ok {
		return r, nil
	}
	var chain []SymbolRef
	visited := map[SymbolRef]bool{}
	cur := ref
	var final SymbolRef
	var tail []SymbolRef
	for {
		if r, ok := b.resolved[cur]; ok {
			// The cached chain begins at cur, which is not yet in chain.
			final = r.Final
			tail = r.Chain
			break
		}
		if visited[cur] {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: alias cycle: %s", ref, chainString(append(chain, cur)))
		}
		visited[cur] = true
		chain = append(chain, cur)
		node := b.byName[cur.Module]
		if node == nil {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: module %q not found (chain: %s)", ref, cur.Module, chainString(chain))
		}
		space := node.space(cur.Kind)
		if space == nil {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: unsupported kind %s", ref, wasm.ExternTypeName(cur.Kind))
		}
		if int(cur.Index) >= len(space) {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: %s index %d out of range in module %q (%d entries) (chain: %s)",
				ref, wasm.ExternTypeName(cur.Kind), cur.Index, cur.Module, len(space), chainString(chain))
		}
		e := space[cur.Index]
		if !e.imported {
			final = cur
			break
		}
		imp := e.imp
		ctx := fmt.Sprintf("module %q import[%d] %s %q.%q", cur.Module, e.impPos, wasm.ExternTypeName(cur.Kind), imp.Module, imp.Name)
		tgt := b.byName[imp.Module]
		if tgt == nil {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: %s: module %q not found (chain: %s)", ref, ctx, imp.Module, chainString(chain))
		}
		var exp *wasm.Export
		for _, x := range tgt.mod.ExportSection {
			if x.Name == imp.Name {
				exp = x
				break
			}
		}
		if exp == nil {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: %s: module %q has no export %q (chain: %s)", ref, ctx, imp.Module, imp.Name, chainString(chain))
		}
		if exp.Type != cur.Kind {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: %s: kind mismatch: export %q of module %q is %s, not %s (chain: %s)",
				ref, ctx, imp.Name, imp.Module, wasm.ExternTypeName(exp.Type), wasm.ExternTypeName(cur.Kind), chainString(chain))
		}
		if tgtSpace := tgt.space(cur.Kind); int(exp.Index) >= len(tgtSpace) {
			return ResolvedSymbol{}, fmt.Errorf("resolving %s: %s: export %q of module %q: %s index %d out of range (%d entries) (chain: %s)",
				ref, ctx, imp.Name, imp.Module, wasm.ExternTypeName(cur.Kind), exp.Index, len(tgtSpace), chainString(chain))
		}
		cur = SymbolRef{imp.Module, cur.Kind, exp.Index}
	}
	full := append(chain, tail...)
	res := ResolvedSymbol{Final: final, Chain: full}
	for i := range chain {
		sub := append([]SymbolRef(nil), chain[i:]...)
		sub = append(sub, tail...)
		b.resolved[chain[i]] = ResolvedSymbol{Final: final, Chain: sub}
	}
	return res, nil
}

// checkImport validates one import against the final definition it resolves to. Intermediate aliases may
// declare looser limits than the real resource, so the final definition is the only trustworthy reference.
func (b *Bundle) checkImport(node *moduleNode, e symEntry, res ResolvedSymbol) error {
	def := b.byName[res.Final.Module]
	ctx := fmt.Sprintf("module %q import[%d] %s %q.%q", node.name, e.impPos, wasm.ExternTypeName(e.imp.Type), e.imp.Module, e.imp.Name)
	chain := chainString(res.Chain)
	switch e.imp.Type {
	case wasm.ExternTypeFunc:
		if int(e.imp.DescFunc) >= len(node.mod.TypeSection) {
			return fmt.Errorf("%s: type index %d out of range (%d types)", ctx, e.imp.DescFunc, len(node.mod.TypeSection))
		}
		req := node.mod.TypeSection[e.imp.DescFunc]
		typeIdx := def.mod.FunctionSection[int(res.Final.Index)-importCount(def.funcs)]
		if int(typeIdx) >= len(def.mod.TypeSection) {
			return fmt.Errorf("%s: %s: type index %d out of range (%d types)", ctx, res.Final, typeIdx, len(def.mod.TypeSection))
		}
		act := def.mod.TypeSection[typeIdx]
		if !req.EqualsSignature(act.Params, act.Results) {
			return fmt.Errorf("%s: signature (%s)->(%s) does not match %s signature (%s)->(%s) (chain: %s)",
				ctx, valueTypesString(req.Params), valueTypesString(req.Results), res.Final,
				valueTypesString(act.Params), valueTypesString(act.Results), chain)
		}
	case wasm.ExternTypeGlobal:
		req := e.imp.DescGlobal
		act := def.mod.GlobalSection[int(res.Final.Index)-importCount(def.globals)].Type
		if req.ValType != act.ValType || req.Mutable != act.Mutable {
			return fmt.Errorf("%s: global type %s does not match %s type %s (chain: %s)",
				ctx, globalTypeString(req), res.Final, globalTypeString(act), chain)
		}
	case wasm.ExternTypeTable:
		req := e.imp.DescTable
		act := def.mod.TableSection[int(res.Final.Index)-importCount(def.tables)]
		if req.Type != act.Type {
			return fmt.Errorf("%s: table type %s does not match %s type %s (chain: %s)",
				ctx, wasm.RefTypeName(req.Type), res.Final, wasm.RefTypeName(act.Type), chain)
		}
		if err := checkLimits(req.Min, req.Max, act.Min, act.Max); err != nil {
			return fmt.Errorf("%s: table limits of %s: %v (chain: %s)", ctx, res.Final, err, chain)
		}
	case wasm.ExternTypeMemory:
		req := e.imp.DescMem
		act := def.mod.MemorySection
		var reqMax, actMax *uint32
		if req.IsMaxEncoded {
			m := req.Max
			reqMax = &m
		}
		if act.IsMaxEncoded {
			m := act.Max
			actMax = &m
		}
		if err := checkLimits(req.Min, reqMax, act.Min, actMax); err != nil {
			return fmt.Errorf("%s: memory limits of %s: %v (chain: %s)", ctx, res.Final, err, chain)
		}
	}
	return nil
}

// checkLimits validates an import's requested limits against the actual limits of the final definition: the
// actual Min must cover the requested Min, and a requested Max requires an actual Max no larger than it.
func checkLimits(reqMin uint32, reqMax *uint32, actMin uint32, actMax *uint32) error {
	if actMin < reqMin {
		return fmt.Errorf("actual min %d is below required min %d", actMin, reqMin)
	}
	if reqMax != nil {
		if actMax == nil {
			return fmt.Errorf("actual has no max, but max %d is required", *reqMax)
		}
		if *actMax > *reqMax {
			return fmt.Errorf("actual max %d exceeds required max %d", *actMax, *reqMax)
		}
	}
	return nil
}

// buildLayouts stacks the active data segments and active funcref element segments of every module onto the
// final resources, in initialization order. Passive and declarative segments take no part.
func (b *Bundle) buildLayouts() error {
	for _, mName := range b.initOrder {
		node := b.byName[mName]
		for i, ds := range node.mod.DataSection {
			if ds.OffsetExpression == nil {
				continue // passive data segment
			}
			if len(node.mems) == 0 {
				return fmt.Errorf("module %q data segment %d: module has no memory", mName, i)
			}
			off, err := b.evalConstExpr(node, ds.OffsetExpression, nil)
			if err != nil {
				return fmt.Errorf("module %q data segment %d: %v", mName, i, err)
			}
			res := b.resolved[SymbolRef{mName, wasm.ExternTypeMemory, 0}]
			mem := b.memByRef[res.Final]
			start, end := uint64(off), uint64(off)+uint64(len(ds.Init))
			if end > mem.capacity {
				return fmt.Errorf("module %q data segment %d: byte range [%d, %d) exceeds capacity %d of %s",
					mName, i, start, end, mem.capacity, mem.id)
			}
			if len(ds.Init) == 0 {
				continue
			}
			mem.addWrite(WriteRecord{
				Resource: mem.id,
				Chain:    append([]SymbolRef(nil), res.Chain...),
				Writer:   mName,
				Segment:  i,
				Start:    start,
				End:      end,
			}, ds.Init)
		}
		for i, es := range node.mod.ElementSection {
			if es.Mode != wasm.ElementModeActive {
				continue // passive and declarative element segments take no part in the static layout.
			}
			if es.Type != wasm.RefTypeFuncref {
				continue // only funcref element segments are applied.
			}
			if int(es.TableIndex) >= len(node.tables) {
				return fmt.Errorf("module %q element segment %d: table index %d out of range (%d tables)",
					mName, i, es.TableIndex, len(node.tables))
			}
			if es.OffsetExpr == nil {
				return fmt.Errorf("module %q element segment %d: missing offset expression", mName, i)
			}
			off, err := b.evalConstExpr(node, es.OffsetExpr, nil)
			if err != nil {
				return fmt.Errorf("module %q element segment %d: %v", mName, i, err)
			}
			res := b.resolved[SymbolRef{mName, wasm.ExternTypeTable, es.TableIndex}]
			tab := b.tabByRef[res.Final]
			start, end := uint64(off), uint64(off)+uint64(len(es.Init))
			if end > tab.capacity {
				return fmt.Errorf("module %q element segment %d: slot range [%d, %d) exceeds capacity %d of %s",
					mName, i, start, end, tab.capacity, tab.id)
			}
			if len(es.Init) == 0 {
				continue
			}
			slots := make([]TableSlot, len(es.Init))
			for j, idxp := range es.Init {
				if idxp == nil {
					slots[j] = TableSlot{Null: true}
					continue
				}
				if int(*idxp) >= len(node.funcs) {
					return fmt.Errorf("module %q element segment %d: function index %d out of range (%d functions)",
						mName, i, *idxp, len(node.funcs))
				}
				slots[j] = TableSlot{Function: b.resolved[SymbolRef{mName, wasm.ExternTypeFunc, *idxp}].Final}
			}
			tab.addWrite(WriteRecord{
				Resource: tab.id,
				Chain:    append([]SymbolRef(nil), res.Chain...),
				Writer:   mName,
				Segment:  i,
				Start:    start,
				End:      end,
			}, slots)
		}
	}
	return nil
}

// evalConstExpr evaluates a constant expression to an offset. Only i32.const and global.get of an imported
// immutable i32 global are supported.
func (b *Bundle) evalConstExpr(node *moduleNode, expr *wasm.ConstantExpression, stack []SymbolRef) (uint32, error) {
	switch expr.Opcode {
	case wasm.OpcodeI32Const:
		v, _, err := leb128.DecodeInt32(bytes.NewReader(expr.Data))
		if err != nil {
			return 0, fmt.Errorf("read i32.const: %v", err)
		}
		// The i32 bit pattern is reinterpreted as uint32; the caller checks the segment end with wide integers.
		return uint32(v), nil
	case wasm.OpcodeGlobalGet:
		idx, _, err := leb128.DecodeUint32(bytes.NewReader(expr.Data))
		if err != nil {
			return 0, fmt.Errorf("read global.get index: %v", err)
		}
		if int(idx) >= len(node.globals) {
			return 0, fmt.Errorf("global.get index %d out of range (%d globals)", idx, len(node.globals))
		}
		return b.evalGlobal(SymbolRef{node.name, wasm.ExternTypeGlobal, idx}, stack)
	default:
		return 0, fmt.Errorf("unsupported offset expression opcode %s", wasm.InstructionName(expr.Opcode))
	}
}

// evalGlobal evaluates the imported immutable i32 global ref to its final value, following initializers across
// module boundaries. stack holds the final globals currently being evaluated, to detect dependency cycles.
func (b *Bundle) evalGlobal(ref SymbolRef, stack []SymbolRef) (uint32, error) {
	node := b.byName[ref.Module]
	e := node.globals[ref.Index]
	if !e.imported {
		return 0, fmt.Errorf("global.get refers to %s, which is not an imported global", ref)
	}
	if gt := e.imp.DescGlobal; gt.Mutable || gt.ValType != wasm.ValueTypeI32 {
		return 0, fmt.Errorf("global.get refers to %s, which is %s, not an immutable i32", ref, globalTypeString(gt))
	}
	res, ok := b.resolved[ref]
	if !ok {
		var err error
		if res, err = b.resolve(ref); err != nil {
			return 0, err
		}
	}
	final := res.Final
	for _, s := range stack {
		if s == final {
			cycle := append(append([]SymbolRef(nil), stack...), final)
			return 0, fmt.Errorf("global dependency cycle: %s", chainString(cycle))
		}
	}
	if v, ok := b.globalVals[final]; ok {
		return v, nil
	}
	def := b.byName[final.Module]
	defIdx := int(final.Index) - importCount(def.globals)
	g := def.mod.GlobalSection[defIdx]
	next := append(append([]SymbolRef(nil), stack...), final)
	v, err := b.evalConstExpr(def, g.Init, next)
	if err != nil {
		return 0, fmt.Errorf("module %q global %d: %v", final.Module, defIdx, err)
	}
	b.globalVals[final] = v
	return v, nil
}

// memoryResource is the final definition of a memory and the layout stacked onto it.
type memoryResource struct {
	id        ResourceID
	capacity  uint64 // initial Min of the final definition, in bytes
	ov        overlay
	writes    []memWrite
	overrides []OverrideRecord
}

type memWrite struct {
	rec  WriteRecord
	data []byte
}

func (r *memoryResource) addWrite(rec WriteRecord, data []byte) {
	r.writes = append(r.writes, memWrite{rec: rec, data: data})
	idx := len(r.writes) - 1
	r.ov.apply(rec.Start, rec.End, idx, func(lo, hi uint64, prev int) {
		p := r.writes[prev].rec
		r.overrides = append(r.overrides, OverrideRecord{
			WriteRecord: WriteRecord{Resource: rec.Resource, Chain: rec.Chain, Writer: rec.Writer, Segment: rec.Segment, Start: lo, End: hi},
			Previous:    SourceRef{Module: p.Writer, Segment: p.Segment},
		})
	})
}

// tableResource is the final definition of a table and the layout stacked onto it.
type tableResource struct {
	id        ResourceID
	capacity  uint64 // initial Min of the final definition, in slots
	ov        overlay
	writes    []tabWrite
	overrides []OverrideRecord
}

type tabWrite struct {
	rec   WriteRecord
	slots []TableSlot
}

func (r *tableResource) addWrite(rec WriteRecord, slots []TableSlot) {
	r.writes = append(r.writes, tabWrite{rec: rec, slots: slots})
	idx := len(r.writes) - 1
	r.ov.apply(rec.Start, rec.End, idx, func(lo, hi uint64, prev int) {
		p := r.writes[prev].rec
		r.overrides = append(r.overrides, OverrideRecord{
			WriteRecord: WriteRecord{Resource: rec.Resource, Chain: rec.Chain, Writer: rec.Writer, Segment: rec.Segment, Start: lo, End: hi},
			Previous:    SourceRef{Module: p.Writer, Segment: p.Segment},
		})
	})
}

// span is one non-overlapping piece of an overlay. write is the index of the write that produced it, or -1 when
// the range was never written (zero in a memory, null in a table).
type span struct {
	start, end uint64
	write      int
	off        uint64 // offset of span start within the write's content
}

// overlay tracks which write covers which part of a resource. It stores one span per source change, so
// capacities and zero holes of any size are never expanded into per-byte or per-slot arrays.
type overlay struct {
	spans []span
}

func newOverlay(capacity uint64) overlay {
	if capacity == 0 {
		return overlay{}
	}
	return overlay{spans: []span{{end: capacity, write: -1}}}
}

// apply replaces [s, e) with a span of the write idx. onOverlap is called for each previously written sub-range
// that gets covered, before it is replaced.
func (o *overlay) apply(s, e uint64, idx int, onOverlap func(lo, hi uint64, prev int)) {
	out := make([]span, 0, len(o.spans)+1)
	for _, sp := range o.spans {
		if sp.end <= s || sp.start >= e {
			out = append(out, sp)
			continue
		}
		lo, hi := sp.start, sp.end
		if lo < s {
			lo = s
		}
		if hi > e {
			hi = e
		}
		if sp.write >= 0 && onOverlap != nil {
			onOverlap(lo, hi, sp.write)
		}
		if sp.start < lo {
			out = append(out, span{sp.start, lo, sp.write, sp.off})
		}
		if hi < sp.end {
			out = append(out, span{hi, sp.end, sp.write, sp.off + (hi - sp.start)})
		}
	}
	pos := sort.Search(len(out), func(i int) bool { return out[i].start >= s })
	out = append(out, span{})
	copy(out[pos+1:], out[pos:])
	out[pos] = span{s, e, idx, 0}
	o.spans = mergeAdjacent(out)
}

// mergeAdjacent joins spans that have the same source: two unwritten spans, or two contiguous spans of the same
// write.
func mergeAdjacent(in []span) []span {
	out := in[:0]
	for _, sp := range in {
		if n := len(out); n > 0 && out[n-1].end == sp.start && out[n-1].write == sp.write {
			if sp.write == -1 || out[n-1].off+(out[n-1].end-out[n-1].start) == sp.off {
				out[n-1].end = sp.end
				continue
			}
		}
		out = append(out, sp)
	}
	return out
}

// clip returns the intersection of sp with [start, end).
func clip(sp span, start, end uint64) (lo, hi uint64, ok bool) {
	if sp.end <= start || sp.start >= end {
		return 0, 0, false
	}
	lo, hi = sp.start, sp.end
	if lo < start {
		lo = start
	}
	if hi > end {
		hi = end
	}
	return lo, hi, true
}

func chainString(chain []SymbolRef) string {
	var sb strings.Builder
	for i, ref := range chain {
		if i > 0 {
			sb.WriteString(" -> ")
		}
		sb.WriteString(ref.String())
	}
	return sb.String()
}

func valueTypesString(ts []wasm.ValueType) string {
	names := make([]string, len(ts))
	for i, t := range ts {
		names[i] = wasm.ValueTypeName(t)
	}
	return strings.Join(names, ",")
}

func globalTypeString(g *wasm.GlobalType) string {
	if g.Mutable {
		return "mutable " + wasm.ValueTypeName(g.ValType)
	}
	return wasm.ValueTypeName(g.ValType)
}
