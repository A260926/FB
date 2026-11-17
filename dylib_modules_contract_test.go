package macho_test

// Contract tests for the LC_DYSYMTAB dylib module/TOC/external-reference
// relationships. All inputs are inert bytes built by task_support/fixture;
// no image is loaded or executed.

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"testing"

	macho "github.com/blacktop/go-macho"
	"github.com/blacktop/go-macho/task_support/fixture"
	"github.com/blacktop/go-macho/task_support/taskcheck"
	"github.com/blacktop/go-macho/types"
)

// variants covers thin/wide images in both file byte orders.
var variants = []struct {
	name string
	wide bool
	big  bool
}{
	{"32-bit little-endian", false, false},
	{"32-bit big-endian", false, true},
	{"64-bit little-endian", true, false},
	{"64-bit big-endian", true, true},
}

func openImage(t *testing.T, buf []byte) *macho.File {
	t.Helper()
	f, err := macho.NewFile(bytes.NewReader(buf))
	if err != nil {
		t.Fatalf("NewFile failed: %v", err)
	}
	t.Cleanup(func() { f.Close() })
	return f
}

func orderOf(big bool) binary.ByteOrder {
	if big {
		return binary.BigEndian
	}
	return binary.LittleEndian
}

// expectedRefs are the (isym, flags) pairs encoded by fixture.Build, in the
// physical order of the referenced symbol table.
var expectedRefPairs = [][2]uint32{{2, 1}, {5, 0}, {4, 1}}

// expectedRawRef gives the on-disk word for each reference pair under the
// file byte order.
func expectedRawRef(pair [2]uint32, big bool) uint32 {
	isym, flags := pair[0], pair[1]
	if big {
		return isym<<8 | flags
	}
	return isym | flags<<24
}

// TestDylibTables verifies physical layout: table sizes and order, the packed
// reference bit field under both byte orders, the 32/64-bit module tail
// mapping, and the ordinary symbol/indirect tables staying unchanged.
func TestDylibTables(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, v := range variants {
			img := fixture.Build(v.wide, v.big, false)
			f := openImage(t, img.Bytes)
			prefix := v.name + ": "

			// Ordinary symbol and indirect tables keep their old meaning.
			c.Check(f.Symtab != nil && len(f.Symtab.Syms) == 6, prefix+"ordinary symbol count")
			c.Check(f.Symtab.Syms[2].Name == "_same" && f.Symtab.Syms[3].Value == 0x200,
				prefix+"ordinary symbol names/values preserved")
			c.Check(len(f.Dysymtab.IndirectSyms) == 3, prefix+"indirect count")
			c.Check(f.Dysymtab.IndirectSyms[0] == 2 &&
				f.Dysymtab.IndirectSyms[1] == types.INDIRECT_SYMBOL_LOCAL &&
				f.Dysymtab.IndirectSyms[2] == 5, prefix+"indirect contents preserved")

			// TOC: physical rows, duplicates retained.
			toc := f.Dysymtab.TOC
			c.Check(len(toc) == 3, prefix+"TOC row count")
			wantTOC := []types.DylibTableOfContents{
				{SymbolIndex: 3, ModuleIndex: 1},
				{SymbolIndex: 2, ModuleIndex: 0},
				{SymbolIndex: 2, ModuleIndex: 0},
			}
			for i, want := range wantTOC {
				c.Check(toc[i] == want, prefix+fmt.Sprintf("TOC row %d fields", i))
			}

			// External references: 24-bit isym + 8-bit flags decoded per byte
			// order, Raw kept as the file-order word.
			refs := f.Dysymtab.ExternalRefs
			c.Check(len(refs) == 3, prefix+"external reference count")
			for i, pair := range expectedRefPairs {
				c.Check(refs[i].SymbolIndex == pair[0] && uint32(refs[i].Flags) == pair[1],
					prefix+fmt.Sprintf("reference %d isym/flags decode", i))
				c.Check(refs[i].Raw == expectedRawRef(pair, v.big),
					prefix+fmt.Sprintf("reference %d raw word", i))
				c.Check(refs[i].Symbol == &f.Symtab.Syms[pair[0]],
					prefix+fmt.Sprintf("reference %d symbol slot", i))
			}

			// Module table: physical index, string-table name, windows, and
			// the meaning-based 32/64-bit Objective-C tail.
			mods := f.Dysymtab.ModuleTable
			c.Check(len(mods) == 2, prefix+"module count")
			wantWindows := []struct{ iext, next, iref, nref, iloc, nloc uint32 }{
				{2, 1, 1, 2, 0, 1},
				{3, 1, 0, 1, 1, 1},
			}
			for i, want := range wantWindows {
				m := mods[i]
				c.Check(m.Index == uint32(i), prefix+fmt.Sprintf("module %d physical index", i))
				c.Check(m.Name == "unit.o", prefix+fmt.Sprintf("module %d string-table name", i))
				c.Check(m.Header.Iextdefsym == want.iext && m.Header.Nextdefsym == want.next,
					prefix+fmt.Sprintf("module %d extdef window", i))
				c.Check(m.Header.Irefsym == want.iref && m.Header.Nrefsym == want.nref,
					prefix+fmt.Sprintf("module %d reference window", i))
				c.Check(m.Header.Ilocalsym == want.iloc && m.Header.Nlocalsym == want.nloc,
					prefix+fmt.Sprintf("module %d local window", i))
				c.Check(m.Header.ObjcModuleInfoSize == 0x50+uint32(i),
					prefix+fmt.Sprintf("module %d objc size meaning", i))
				if v.wide {
					// 64-bit tail: size then 64-bit address.
					c.Check(m.Header.ObjcModuleInfoAddr == 0xB000+uint64(i),
						prefix+fmt.Sprintf("module %d 64-bit objc address", i))
				} else {
					// 32-bit tail: address then size, address widened.
					c.Check(m.Header.ObjcModuleInfoAddr == 0xA000+uint64(i),
						prefix+fmt.Sprintf("module %d widened 32-bit objc address", i))
				}
			}

			// The fixed-shift types.DylibReference helper models the
			// big-endian bit field; on a little-endian word it disagrees with
			// the correct decode, which is why the new model decodes per byte
			// order instead of reusing that shift blindly.
			if !v.big {
				legacy := types.DylibReference(refs[0].Raw)
				c.Check(legacy.SymIndex() != refs[0].SymbolIndex ||
					uint32(legacy.Flags()) != uint32(refs[0].Flags),
					prefix+"fixed-shift helper is not reused for little-endian words")
			}
		}
	})
}

// TestDylibOwnership verifies that every relationship aliases the original
// Symtab.Syms slots, that duplicate names and rows are never merged, and that
// reference windows keep their physical order even when they are not laid out
// in module order.
func TestDylibOwnership(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, v := range variants {
			img := fixture.Build(v.wide, v.big, false)
			f := openImage(t, img.Bytes)
			prefix := v.name + ": "
			syms := f.Symtab.Syms
			mods := f.Dysymtab.ModuleTable
			refs := f.Dysymtab.ExternalRefs

			// Same-named modules stay distinct physical entries.
			c.Check(mods[0].Name == mods[1].Name && mods[0].Name == "unit.o",
				prefix+"same-named modules both present")
			c.Check(mods[0].Index == 0 && mods[1].Index == 1,
				prefix+"same-named modules keep distinct indices")

			// Same-named symbols stay distinct slots with distinct values.
			c.Check(syms[2].Name == syms[3].Name && syms[2].Name == "_same",
				prefix+"same-named definitions both present")
			c.Check(syms[2].Value == 0x100 && syms[3].Value == 0x200,
				prefix+"same-named definitions keep distinct values")

			// Nlist windows point at the original backing array slots.
			c.Check(len(mods[0].ExternalDefinitions) == 1 &&
				mods[0].ExternalDefinitions[0] == &syms[2],
				prefix+"module 0 extdef slot identity")
			c.Check(len(mods[1].ExternalDefinitions) == 1 &&
				mods[1].ExternalDefinitions[0] == &syms[3],
				prefix+"module 1 extdef slot identity")
			c.Check(mods[0].LocalSymbols[0] == &syms[0] &&
				mods[1].LocalSymbols[0] == &syms[1],
				prefix+"module local slot identity")

			// Reference windows are not in module order: module 0 owns
			// ExternalRefs[1:3], module 1 owns ExternalRefs[0].
			c.Check(len(mods[0].References) == 2 && len(mods[1].References) == 1,
				prefix+"reference window sizes")
			c.Check(mods[0].References[0] == refs[1] && mods[0].References[1] == refs[2],
				prefix+"module 0 reference window order")
			c.Check(mods[1].References[0] == refs[0],
				prefix+"module 1 reference window order")
			c.Check(mods[0].References[0].Symbol == &syms[5] &&
				mods[0].References[1].Symbol == &syms[4] &&
				mods[1].References[0].Symbol == &syms[2],
				prefix+"reference symbols alias original slots")

			// TOC rows are not grouped by module: module 0's rows arrive
			// second and third, and the duplicated row survives as two
			// pointers to the same original slot.
			c.Check(len(mods[0].TOCSymbols) == 2 && len(mods[1].TOCSymbols) == 1,
				prefix+"per-module TOC row counts with duplicates retained")
			c.Check(mods[0].TOCSymbols[0] == &syms[2] &&
				mods[0].TOCSymbols[1] == &syms[2],
				prefix+"duplicate TOC rows preserve order and slot identity")
			c.Check(mods[1].TOCSymbols[0] == &syms[3],
				prefix+"out-of-group TOC row assigned to its module")
		}
	})
}

// TestDylibConsistency verifies command-order independence, failure results
// for every class of malformed input, and the zero-table/no-dysym cases.
func TestDylibConsistency(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		for _, v := range variants {
			bo := orderOf(v.big)
			prefix := v.name + ": "

			// Reversed LC_SYMTAB/LC_DYSYMTAB order produces the same model.
			forward := openImage(t, fixture.Build(v.wide, v.big, false).Bytes)
			reversedImg := fixture.Build(v.wide, v.big, true)
			reversed := openImage(t, reversedImg.Bytes)
			c.Check(len(reversed.Dysymtab.TOC) == len(forward.Dysymtab.TOC) &&
				reversed.Dysymtab.TOC[0] == forward.Dysymtab.TOC[0],
				prefix+"TOC identical after command reorder")
			c.Check(len(reversed.Dysymtab.ModuleTable) == len(forward.Dysymtab.ModuleTable) &&
				reversed.Dysymtab.ModuleTable[1].Name == forward.Dysymtab.ModuleTable[1].Name,
				prefix+"modules identical after command reorder")
			c.Check(reversed.Dysymtab.ExternalRefs[2].Raw == forward.Dysymtab.ExternalRefs[2].Raw,
				prefix+"references identical after command reorder")

			// Global undefined-symbol window checks still fail when
			// LC_DYSYMTAB comes first: patch Iundefsym (offset 24 inside the
			// first 80-byte command following the file header).
			bad := append([]byte(nil), reversedImg.Bytes...)
			hdrSize := uint32(types.FileHeaderSize32)
			if v.wide {
				hdrSize = types.FileHeaderSize64
			}
			bo.PutUint32(bad[hdrSize+24:], 99)
			if g, err := macho.NewFile(bytes.NewReader(bad)); err == nil {
				g.Close()
				c.Check(false, prefix+"bad global undefined window rejected when dysymtab is first")
			} else {
				c.Check(g == nil, prefix+"reversed bad window returns no partial file")
			}

			// A shortened image fails rather than returning partial tables;
			// whichever link-edit table the shortened bytes no longer cover
			// rejects the read.
			img := fixture.Build(v.wide, v.big, false)
			truncated := append([]byte(nil), img.Bytes[:img.Modoff+4]...)
			if g, err := macho.NewFile(bytes.NewReader(truncated)); err == nil {
				g.Close()
				c.Check(false, prefix+"truncated image rejected")
			} else {
				c.Check(g == nil, prefix+"truncated image returns no partial file")
			}

			// In normal command order LC_SYMTAB (24 bytes) precedes
			// LC_DYSYMTAB; relocate each table offset near the image end so
			// each table's own read runs out of bytes.
			dysBase := hdrSize + 24
			relocate := func(fieldOff uint32) []byte {
				b := append([]byte(nil), img.Bytes...)
				bo.PutUint32(b[dysBase+fieldOff:], 0x8f8)
				return b
			}
			if g, err := macho.NewFile(bytes.NewReader(relocate(32))); err == nil { // Tocoffset
				g.Close()
				c.Check(false, prefix+"missing TOC bytes rejected")
			} else {
				c.Check(g == nil, prefix+"missing TOC bytes return no partial file")
			}
			if g, err := macho.NewFile(bytes.NewReader(relocate(40))); err == nil { // Modtaboff
				g.Close()
				c.Check(false, prefix+"missing module table bytes rejected")
			} else {
				c.Check(g == nil, prefix+"missing module bytes return no partial file")
			}
			if g, err := macho.NewFile(bytes.NewReader(relocate(48))); err == nil { // Extrefsymoff
				g.Close()
				c.Check(false, prefix+"missing reference table bytes rejected")
			} else {
				c.Check(g == nil, prefix+"missing reference bytes return no partial file")
			}

			// TOC module index that does not link to a module.
			bad = append([]byte(nil), img.Bytes...)
			bo.PutUint32(bad[img.Tocoff+4:], 9)
			if g, err := macho.NewFile(bytes.NewReader(bad)); err == nil {
				g.Close()
				c.Check(false, prefix+"TOC row with bad module index rejected")
			} else {
				c.Check(g == nil, prefix+"bad TOC module index returns no partial file")
			}

			// TOC symbol outside the named module's external-definition window.
			bad = append([]byte(nil), img.Bytes...)
			bo.PutUint32(bad[img.Tocoff:], 1)
			if g, err := macho.NewFile(bytes.NewReader(bad)); err == nil {
				g.Close()
				c.Check(false, prefix+"TOC symbol outside module extdef window rejected")
			} else {
				c.Check(g == nil, prefix+"misowned TOC symbol returns no partial file")
			}

			// Reference isym that does not link into the symbol table.
			bad = append([]byte(nil), img.Bytes...)
			if v.big {
				bo.PutUint32(bad[img.Refoff:], 99<<8)
			} else {
				bo.PutUint32(bad[img.Refoff:], 99)
			}
			if g, err := macho.NewFile(bytes.NewReader(bad)); err == nil {
				g.Close()
				c.Check(false, prefix+"reference with bad isym rejected")
			} else {
				c.Check(g == nil, prefix+"bad reference isym returns no partial file")
			}

			// Module local window outside the nlist table (Ilocalsym is the
			// sixth u32 of a module record, offset 20).
			bad = append([]byte(nil), img.Bytes...)
			bo.PutUint32(bad[img.Modoff+20:], 100)
			if g, err := macho.NewFile(bytes.NewReader(bad)); err == nil {
				g.Close()
				c.Check(false, prefix+"module window outside nlist rejected")
			} else {
				c.Check(g == nil, prefix+"bad module window returns no partial file")
			}

			// Module name without a NUL terminator: shrink Strsize (LC_SYMTAB
			// field at command offset 20) so the terminating NUL of "unit.o"
			// falls outside the declared string table.
			bad = append([]byte(nil), img.Bytes...)
			bo.PutUint32(bad[hdrSize+20:], 24)
			if g, err := macho.NewFile(bytes.NewReader(bad)); err == nil {
				g.Close()
				c.Check(false, prefix+"unterminated module name rejected")
			} else {
				c.Check(g == nil, prefix+"unterminated module name returns no partial file")
			}

			// Zero-count tables never touch their recorded (garbage) offsets.
			empty := openImage(t, fixture.BuildEmptyTables(v.wide, v.big).Bytes)
			c.Check(empty.Dysymtab != nil, prefix+"zero-table image parses")
			c.Check(len(empty.Dysymtab.TOC) == 0 && len(empty.Dysymtab.ModuleTable) == 0 &&
				len(empty.Dysymtab.ExternalRefs) == 0 && len(empty.Dysymtab.IndirectSyms) == 0,
				prefix+"zero-count tables expose empty slices without reading offsets")
			c.Check(len(empty.Symtab.Syms) == 1, prefix+"ordinary symbols unaffected by zero tables")

			// No LC_DYSYMTAB at all: ordinary symbol path unchanged.
			noDys := openImage(t, fixture.BuildNoDysym(v.wide, v.big).Bytes)
			c.Check(noDys.Dysymtab == nil, prefix+"missing LC_DYSYMTAB leaves Dysymtab nil")
			c.Check(noDys.Symtab != nil && noDys.Symtab.Syms[0].Name == "_lonely",
				prefix+"missing LC_DYSYMTAB keeps ordinary symbols")
		}
	})
}

// ExampleDysymtab_moduleTables shows walking the resolved module relationships
// published directly by NewFile.
func ExampleDysymtab_moduleTables() {
	img := fixture.Build(true, false, false)
	f, err := macho.NewFile(bytes.NewReader(img.Bytes))
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	defer f.Close()
	for _, m := range f.Dysymtab.ModuleTable {
		fmt.Printf("module %d %q defs=%d locals=%d refs=%d toc=%d\n",
			m.Index, m.Name, len(m.ExternalDefinitions), len(m.LocalSymbols),
			len(m.References), len(m.TOCSymbols))
	}
	// Output:
	// module 0 "unit.o" defs=1 locals=1 refs=2 toc=2
	// module 1 "unit.o" defs=1 locals=1 refs=1 toc=1
}
