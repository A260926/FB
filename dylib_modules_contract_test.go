package macho_test

// Contract tests for the LC_DYSYMTAB dylib tables (table of contents, module
// table and referenced symbol table) published by macho.NewFile. Every input
// is built in-memory by task_support/fixture from inert bytes: no image is
// loaded, executed or rewritten.

import (
	"bytes"
	"encoding/binary"
	"testing"

	"github.com/blacktop/go-macho/task_support/fixture"
	"github.com/blacktop/go-macho/task_support/taskcheck"
	"github.com/blacktop/go-macho/types"

	macho "github.com/blacktop/go-macho"
)

// Field offsets inside struct dysymtab_command (all uint32 fields).
const (
	dyIundefsym    = 24
	dyNundefsym    = 28
	dyTocoffset    = 32
	dyNtoc         = 36
	dyModtaboff    = 40
	dyNmodtab      = 44
	dyExtrefsymoff = 48
	dyNextrefsyms  = 52
)

func boName(big bool) string {
	if big {
		return "big-endian"
	}
	return "little-endian"
}

func widthName(wide bool) string {
	if wide {
		return "64-bit"
	}
	return "32-bit"
}

// TestDylibTables verifies the physical layout: the TOC rows, module headers,
// widened 32/64-bit tails, packed initializer fields and the byte-order
// dependent 24/8-bit reference words across every format and command order.
func TestDylibTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, wide := range []bool{false, true} {
			for _, big := range []bool{false, true} {
				for _, dysymFirst := range []bool{false, true} {
					label := widthName(wide) + "/" + boName(big)
					if dysymFirst {
						label += "/dysymtab-first"
					}
					img := fixture.Build(wide, big, dysymFirst)
					f, err := macho.NewFile(bytes.NewReader(img.Bytes))
					c.Check(err == nil, "%s: NewFile returned error: %v", label, err)
					if err != nil {
						continue
					}
					defer f.Close()
					bo := f.ByteOrder
					dys := f.Dysymtab
					c.Check(dys != nil, "%s: Dysymtab missing", label)

					// TOC: three rows in file order, duplicate rows retained.
					c.Check(len(dys.TOC) == 3, "%s: TOC row count = %d, want 3", label, len(dys.TOC))
					wantTOC := []types.DylibTableOfContents{
						{SymbolIndex: 3, ModuleIndex: 1},
						{SymbolIndex: 2, ModuleIndex: 0},
						{SymbolIndex: 2, ModuleIndex: 0},
					}
					for i, want := range wantTOC {
						c.Check(dys.TOC[i] == want, "%s: TOC[%d] = %+v, want %+v", label, i, dys.TOC[i], want)
					}

					// External references: Raw is the word read in file byte
					// order; the 24-bit index and 8-bit flags follow endianness.
					wantRefs := []struct {
						isym  uint32
						flags uint8
					}{{2, 1}, {5, 0}, {4, 1}}
					c.Check(len(dys.ExternalRefs) == 3, "%s: ref count = %d, want 3", label, len(dys.ExternalRefs))
					for i, want := range wantRefs {
						ref := dys.ExternalRefs[i]
						c.Check(ref.SymbolIndex == want.isym, "%s: ref[%d] isym = %d, want %d", label, i, ref.SymbolIndex, want.isym)
						c.Check(ref.Flags == want.flags, "%s: ref[%d] flags = %d, want %d", label, i, ref.Flags, want.flags)
						c.Check(ref.Raw == fixture.EncodeReferenceWord(big, want.isym, want.flags),
							"%s: ref[%d] Raw = %#x, want the on-disk word", label, i, ref.Raw)
						// Raw must match the physical bytes at Refoff.
						phys := bo.Uint32(img.Bytes[int(img.Refoff)+i*4:])
						c.Check(ref.Raw == phys, "%s: ref[%d] Raw %#x != physical word %#x", label, i, ref.Raw, phys)
					}
					// The legacy fixed-shift helper describes the big-endian
					// layout; the new model must not reuse it for little-endian.
					legacy := types.DylibReference(dys.ExternalRefs[0].Raw)
					if big {
						c.Check(legacy.SymIndex() == dys.ExternalRefs[0].SymbolIndex,
							"%s: legacy helper disagrees on big-endian isym", label)
						c.Check(legacy.Flags() == dys.ExternalRefs[0].Flags,
							"%s: legacy helper disagrees on big-endian flags", label)
					} else {
						c.Check(legacy.SymIndex() != dys.ExternalRefs[0].SymbolIndex,
							"%s: little-endian isym must not use the fixed big-endian shift", label)
						c.Check(dys.ExternalRefs[0].SymbolIndex == 2,
							"%s: little-endian isym = %d, want 2", label, dys.ExternalRefs[0].SymbolIndex)
					}

					// Module table: physical order, zero-based index, duplicate
					// module names kept as separate records.
					c.Check(len(dys.ModuleTable) == 2, "%s: module count = %d, want 2", label, len(dys.ModuleTable))
					wantWindows := []struct {
						iext, next, iref, nref, iloc, nloc uint32
					}{
						{2, 1, 1, 2, 0, 1},
						{3, 1, 0, 1, 1, 1},
					}
					for i, m := range dys.ModuleTable {
						c.Check(m.Index == uint32(i), "%s: module %d Index = %d", label, i, m.Index)
						c.Check(m.Name == "unit.o", "%s: module %d Name = %q", label, i, m.Name)
						c.Check(m.Header.ModuleName == fixture.StrModule,
							"%s: module %d ModuleName offset = %d, want %d", label, i, m.Header.ModuleName, fixture.StrModule)
						w := wantWindows[i]
						c.Check(m.Header.Iextdefsym == w.iext && m.Header.Nextdefsym == w.next,
							"%s: module %d extdef window = (%d,%d), want (%d,%d)", label, i,
							m.Header.Iextdefsym, m.Header.Nextdefsym, w.iext, w.next)
						c.Check(m.Header.Irefsym == w.iref && m.Header.Nrefsym == w.nref,
							"%s: module %d ref window = (%d,%d), want (%d,%d)", label, i,
							m.Header.Irefsym, m.Header.Nrefsym, w.iref, w.nref)
						c.Check(m.Header.Ilocalsym == w.iloc && m.Header.Nlocalsym == w.nloc,
							"%s: module %d local window = (%d,%d), want (%d,%d)", label, i,
							m.Header.Ilocalsym, m.Header.Nlocalsym, w.iloc, w.nloc)
						// Packed initializer/terminator fields copied verbatim.
						c.Check(m.Header.IinitIterm == fixture.ModuleIinitIterm,
							"%s: module %d IinitIterm = %#x", label, i, m.Header.IinitIterm)
						c.Check(m.Header.NinitNterm == fixture.ModuleNinitNterm,
							"%s: module %d NinitNterm = %#x", label, i, m.Header.NinitNterm)
						// Tail mapped by meaning across the distinct layouts.
						c.Check(m.Header.ObjcModuleInfoSize == fixture.ModuleObjcSize,
							"%s: module %d ObjcModuleInfoSize = %#x", label, i, m.Header.ObjcModuleInfoSize)
						if wide {
							c.Check(m.Header.ObjcModuleInfoAddr == fixture.ModuleObjcAddr64,
								"%s: 64-bit module %d ObjcModuleInfoAddr = %#x", label, i, m.Header.ObjcModuleInfoAddr)
						} else {
							c.Check(m.Header.ObjcModuleInfoAddr == uint64(fixture.ModuleObjcAddr32),
								"%s: 32-bit module %d widened ObjcModuleInfoAddr = %#x", label, i, m.Header.ObjcModuleInfoAddr)
						}
					}

					// The indirect table keeps its previous meaning.
					c.Check(len(dys.IndirectSyms) == 3 &&
						dys.IndirectSyms[0] == 2 &&
						dys.IndirectSyms[1] == types.INDIRECT_SYMBOL_LOCAL &&
						dys.IndirectSyms[2] == 5,
						"%s: IndirectSyms = %v", label, dys.IndirectSyms)
				}
			}
		}
	})
}

// TestDylibOwnership verifies that every linked Symbol is the exact slot in
// f.Symtab.Syms, that names are never identities, and that TOC/reference
// ordering (including duplicates and windows that are not grouped by module)
// is preserved. Reversed command order must produce the same model.
func TestDylibOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		img := fixture.Build(true, false, false)
		f, err := macho.NewFile(bytes.NewReader(img.Bytes))
		c.Check(err == nil, "NewFile: %v", err)
		if err != nil {
			return
		}
		defer f.Close()
		dys := f.Dysymtab

		// Same-named symbols stay distinct slots and are never merged.
		c.Check(&f.Symtab.Syms[2] != &f.Symtab.Syms[3], "same-named nlist slots must be distinct")
		c.Check(f.Symtab.Syms[2].Name == f.Symtab.Syms[3].Name && f.Symtab.Syms[2].Name == "_same",
			"duplicate definition name = %q/%q", f.Symtab.Syms[2].Name, f.Symtab.Syms[3].Name)
		c.Check(f.Symtab.Syms[2].Value == 0x100 && f.Symtab.Syms[3].Value == 0x200,
			"duplicate definitions keep distinct values %#x/%#x", f.Symtab.Syms[2].Value, f.Symtab.Syms[3].Value)

		// External reference Symbol pointers own the corresponding nlist slot.
		wantRefIsym := []uint32{2, 5, 4}
		for i, isym := range wantRefIsym {
			c.Check(dys.ExternalRefs[i].Symbol == &f.Symtab.Syms[isym],
				"ExternalRefs[%d].Symbol does not address Symtab.Syms[%d]", i, isym)
		}

		// Module windows are absolute nlist (or reference) windows. Reference
		// windows deliberately do not follow module order: module 0 owns
		// ExternalRefs[1:3], module 1 owns ExternalRefs[0].
		m0, m1 := dys.ModuleTable[0], dys.ModuleTable[1]
		c.Check(m0.ExternalDefinitions[0] == &f.Symtab.Syms[2], "module 0 extdef slot")
		c.Check(m1.ExternalDefinitions[0] == &f.Symtab.Syms[3], "module 1 extdef slot")
		c.Check(m0.LocalSymbols[0] == &f.Symtab.Syms[0], "module 0 local slot")
		c.Check(m1.LocalSymbols[0] == &f.Symtab.Syms[1], "module 1 local slot")
		c.Check(len(m0.References) == 2 && len(m1.References) == 1, "module reference window lengths")
		c.Check(m0.References[0] == dys.ExternalRefs[1] && m0.References[1] == dys.ExternalRefs[2],
			"module 0 keeps reference window order and values")
		c.Check(m1.References[0] == dys.ExternalRefs[0], "module 1 keeps reference window order and values")

		// TOC rows are not grouped by module: module 1's row comes first in the
		// physical TOC. TOCSymbols preserves per-module subsequence order, and
		// the repeated row points at the same slot twice.
		c.Check(dys.TOC[0].ModuleIndex == 1, "TOC is not assumed to be grouped by module")
		c.Check(len(m0.TOCSymbols) == 2, "module 0 TOC symbol count = %d", len(m0.TOCSymbols))
		c.Check(m0.TOCSymbols[0] == &f.Symtab.Syms[2] && m0.TOCSymbols[1] == &f.Symtab.Syms[2],
			"duplicate TOC rows retain order and the same nlist slot")
		c.Check(len(m1.TOCSymbols) == 1 && m1.TOCSymbols[0] == &f.Symtab.Syms[3],
			"module 1 TOC symbol must address Symtab.Syms[3]")

		// Same-named modules are not merged.
		c.Check(m0.Name == m1.Name && m0.Index != m1.Index, "same-named modules stay separate records")

		// Reversed command order yields an identical resolved model, compared
		// by indices/names (pointer slots belong to a different File).
		rev := fixture.Build(true, false, true)
		g, revErr := macho.NewFile(bytes.NewReader(rev.Bytes))
		c.Check(revErr == nil, "reversed command order: %v", revErr)
		if revErr != nil {
			return
		}
		defer g.Close()
		c.Check(len(g.Dysymtab.ModuleTable) == len(dys.ModuleTable) &&
			len(g.Dysymtab.TOC) == len(dys.TOC) &&
			len(g.Dysymtab.ExternalRefs) == len(dys.ExternalRefs),
			"reversed order table lengths differ")
		for i := range dys.TOC {
			c.Check(g.Dysymtab.TOC[i] == dys.TOC[i], "reversed order TOC[%d] differs", i)
		}
		for i := range dys.ExternalRefs {
			a, b := dys.ExternalRefs[i], g.Dysymtab.ExternalRefs[i]
			c.Check(a.Raw == b.Raw && a.SymbolIndex == b.SymbolIndex && a.Flags == b.Flags,
				"reversed order ExternalRefs[%d] differs", i)
			c.Check(g.Dysymtab.ExternalRefs[i].Symbol == &g.Symtab.Syms[b.SymbolIndex],
				"reversed order ref[%d] does not own its nlist slot", i)
		}
		for i := range dys.ModuleTable {
			c.Check(g.Dysymtab.ModuleTable[i].Header == dys.ModuleTable[i].Header,
				"reversed order module %d header differs", i)
			c.Check(g.Dysymtab.ModuleTable[i].Name == dys.ModuleTable[i].Name,
				"reversed order module %d name differs", i)
		}
	})
}

// mutateDysymtab writes one uint32 field of the LC_DYSYMTAB command in place.
func mutateDysymtab(img fixture.Image, wide, dysymFirst bool, bo binary.ByteOrder, fieldOff, value uint32) {
	base := fixture.DysymtabCmdOffset(wide, dysymFirst)
	bo.PutUint32(img.Bytes[base+int(fieldOff):], value)
}

// checkNewFileFails asserts NewFile rejects the input and returns no File.
func checkNewFileFails(c *taskcheck.C, label string, data []byte) {
	f, err := macho.NewFile(bytes.NewReader(data))
	c.Check(err != nil && f == nil, "%s must fail (err=%v)", label, err)
}

// TestDylibConsistency verifies that truncated bytes, broken windows and bad
// links all fail NewFile (with no partial File), that zero-count tables are
// never read, and that files without LC_DYSYMTAB still parse normally.
func TestDylibConsistency(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		bo := binary.LittleEndian

		// Truncation at each nonempty table must fail rather than return
		// partially built relationships.
		good := fixture.Build(true, false, false)
		cuts := map[string]uint32{
			"TOC":      good.Tocoff,
			"modtable": good.Modoff,
			"reftable": good.Refoff,
			"indirect": good.Indoff,
			"nlist":    good.Symoff,
			"strtable": good.Stroff,
		}
		for name, cut := range cuts {
			checkNewFileFails(c, "truncation at "+name, good.Bytes[:cut])
		}

		// External reference isym out of nlist range.
		img := fixture.Build(true, false, false)
		bo.PutUint32(img.Bytes[int(img.Refoff):], fixture.EncodeReferenceWord(false, 99, 0))
		checkNewFileFails(c, "bad reference isym", img.Bytes)

		// TOC module index out of range.
		img = fixture.Build(true, false, false)
		bo.PutUint32(img.Bytes[int(img.Tocoff)+4:], 7)
		checkNewFileFails(c, "bad TOC module index", img.Bytes)

		// TOC symbol outside the referenced module's external-definition window.
		img = fixture.Build(true, false, false)
		bo.PutUint32(img.Bytes[int(img.Tocoff):], 0) // row 0 stays module 1, now local symbol 0
		checkNewFileFails(c, "TOC symbol outside extdef window", img.Bytes)

		// Module reference window outside the external reference table.
		img = fixture.Build(true, false, false)
		bo.PutUint32(img.Bytes[int(img.Modoff)+12:], 99) // module 0 Irefsym
		checkNewFileFails(c, "bad module reference window", img.Bytes)

		// Module extdef window outside the global extdef window.
		img = fixture.Build(true, false, false)
		bo.PutUint32(img.Bytes[int(img.Modoff)+4:], 99) // module 0 Iextdefsym
		checkNewFileFails(c, "bad module extdef window", img.Bytes)

		// Module name offset beyond the string table.
		img = fixture.Build(true, false, false)
		bo.PutUint32(img.Bytes[int(img.Modoff):], 0xfffffff0)
		checkNewFileFails(c, "module name offset out of range", img.Bytes)

		// Module name without a NUL terminator inside Strsize.
		img = fixture.Build(true, false, false)
		img.Bytes[int(img.Stroff)+int(fixture.StrModule)+len("unit.o")] = 'X' // overwrite the NUL
		checkNewFileFails(c, "unterminated module name", img.Bytes)

		// The global undefined-symbol window check survives command reordering.
		for _, dysymFirst := range []bool{false, true} {
			bad := fixture.Build(false, false, dysymFirst)
			mutateDysymtab(bad, false, dysymFirst, bo, dyIundefsym, 99)
			checkNewFileFails(c, "Iundefsym out of range (dysymFirst)", bad.Bytes)
		}

		// Zero-count tables: their recorded offsets are never read, so wild
		// offsets are harmless and the relationship slices stay empty.
		empty := fixture.Build(true, false, false)
		mutateDysymtab(empty, true, false, bo, dyNtoc, 0)
		mutateDysymtab(empty, true, false, bo, dyNmodtab, 0)
		mutateDysymtab(empty, true, false, bo, dyNextrefsyms, 0)
		mutateDysymtab(empty, true, false, bo, dyTocoffset, 0xfffff000)
		mutateDysymtab(empty, true, false, bo, dyModtaboff, 0xfffff000)
		mutateDysymtab(empty, true, false, bo, dyExtrefsymoff, 0xfffff000)
		f, err := macho.NewFile(bytes.NewReader(empty.Bytes))
		c.Check(err == nil, "zero-count tables must not read their offsets: %v", err)
		if err == nil {
			c.Check(f.Dysymtab.TOC == nil && f.Dysymtab.ModuleTable == nil && f.Dysymtab.ExternalRefs == nil,
				"zero-count tables must publish empty slices")
			c.Check(len(f.Dysymtab.IndirectSyms) == 3, "indirect table unaffected by empty dylib tables")
			f.Close()
		}

		// A file with no LC_DYSYMTAB still parses through the ordinary path.
		plain := buildSymtabOnlyImage(true, false)
		g, err := macho.NewFile(bytes.NewReader(plain))
		c.Check(err == nil, "symtab-only image must parse: %v", err)
		if err == nil {
			c.Check(g.Dysymtab == nil, "symtab-only image must not synthesize a Dysymtab")
			c.Check(len(g.Symtab.Syms) == 1 && g.Symtab.Syms[0].Name == "_x",
				"ordinary symbol path unchanged without LC_DYSYMTAB")
			g.Close()
		}
	})
}

// buildSymtabOnlyImage emits a minimal thin Mach-O carrying only LC_SYMTAB
// (one defined nlist and its string table), with no LC_DYSYMTAB at all.
func buildSymtabOnlyImage(wide, big bool) []byte {
	var order binary.ByteOrder = binary.LittleEndian
	if big {
		order = binary.BigEndian
	}
	hdrSize := types.FileHeaderSize32
	if wide {
		hdrSize = types.FileHeaderSize64
	}
	const symoff = 0x100
	const stroff = 0x180
	buf := make([]byte, 0x200)
	hdr := types.FileHeader{
		Magic: types.Magic32, CPU: types.CPUX86, Type: types.MH_DYLIB,
		NCommands: 1, SizeCommands: 24,
	}
	if wide {
		hdr.Magic = types.Magic64
		hdr.CPU = types.CPUAmd64
	}
	hdr.Put(buf, order)
	cmd := types.SymtabCmd{LoadCmd: types.LC_SYMTAB, Len: 24, Symoff: symoff, Nsyms: 1, Stroff: stroff, Strsize: 4}
	order.PutUint32(buf[hdrSize:], uint32(cmd.LoadCmd))
	order.PutUint32(buf[hdrSize+4:], cmd.Len)
	order.PutUint32(buf[hdrSize+8:], cmd.Symoff)
	order.PutUint32(buf[hdrSize+12:], cmd.Nsyms)
	order.PutUint32(buf[hdrSize+16:], cmd.Stroff)
	order.PutUint32(buf[hdrSize+20:], cmd.Strsize)
	// nlist: name offset 1, N_SECT in section 1, value 0x4000.
	order.PutUint32(buf[symoff:], 1)
	buf[symoff+4] = byte(types.N_SECT)
	buf[symoff+5] = 1
	order.PutUint16(buf[symoff+6:], 0)
	if wide {
		order.PutUint64(buf[symoff+8:], 0x4000)
	} else {
		order.PutUint32(buf[symoff+8:], 0x4000)
	}
	// string table: NUL, "_x", NUL, padding NUL.
	buf[stroff] = 0
	buf[stroff+1] = '_'
	buf[stroff+2] = 'x'
	buf[stroff+3] = 0
	return buf
}
