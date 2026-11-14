// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package macho

// Read-only program-counter ownership across a fileset hierarchy.

// PCOwnership attributes a program counter to one leaf module of a fileset
// hierarchy. A PC can be owned by more than one module when images map
// overlapping virtual ranges; every match is returned.
type PCOwnership struct {
	// Path is the sequence of LC_FILESET_ENTRY identifiers leading from the
	// File the query started on to the owning leaf module. It is empty for a
	// match in a standalone, non-fileset File.
	Path []string
	// Module is the leaf File whose section contains the queried PC.
	Module *File
	// SectionIndex is the zero-based index of the owning section in
	// Module.Sections (the Mach-O section number is SectionIndex + 1).
	SectionIndex int
	// Section is the owning section.
	Section *Section
	// SectionOffset is the offset of the PC inside the section, i.e.
	// PC - Section.Addr.
	SectionOffset uint64
	// FileOffset is the container-relative file byte offset of the PC. It is
	// zero when Zerofill is true, as zerofill sections occupy no file bytes.
	FileOffset uint64
	// Zerofill reports that the owning section is a zerofill section: the PC
	// is still attributed to it, but no file bytes exist.
	Zerofill bool
	// Symbols holds the candidate symbols of the owning module: N_SECT
	// definitions in the owning section whose value is the greatest not
	// exceeding the PC. All definitions aliasing that address are included,
	// in symbol-table order. Symbols from other sections, sibling modules or
	// non-section symbols are never used to fill gaps.
	Symbols []Symbol
}

// PCOwnership determines which leaf module, section and symbols own pc.
//
// A leaf module is a File without LC_FILESET_ENTRY commands; interior
// fileset Files are traversed and never reported themselves. The match uses
// each leaf section's half-open virtual interval
// [Section.Addr, Section.Addr+Section.Size); overlapping leaf modules all
// produce a result. A standalone, non-fileset File is itself a leaf.
//
// It is a read-only inspection: it never modifies the image and only opens
// fileset entries that the traversal actually reaches.
func (f *File) PCOwnership(pc uint64) []PCOwnership {
	var out []PCOwnership
	f.pcOwnership(pc, nil, &out)
	return out
}

func (f *File) pcOwnership(pc uint64, path []string, out *[]PCOwnership) {
	// Interior fileset node: descend into every entry so that all matching
	// leaves, including modules mapped at overlapping virtual addresses, are
	// retained.
	if len(f.entries) > 0 {
		for _, entry := range f.entries {
			child, err := f.OpenEntry(entry.ID)
			if err != nil {
				continue
			}
			child.pcOwnership(pc, append(path, entry.ID), out)
		}
		return
	}

	// Leaf module: attribute the PC using the sections' half-open intervals.
	for idx, sec := range f.Sections {
		if pc < sec.Addr || pc >= sec.Addr+sec.Size {
			continue
		}
		zerofill := isZerofillSection(sec.Flags)
		owner := PCOwnership{
			Path:          append([]string(nil), path...),
			Module:        f,
			SectionIndex:  idx,
			Section:       sec,
			SectionOffset: pc - sec.Addr,
			Zerofill:      zerofill,
			Symbols:       f.sectionSymbolsAt(uint8(idx+1), sec, pc),
		}
		if !zerofill {
			// Section.Offset is container-relative, and the displacement of
			// the PC inside the section is the same in the file.
			owner.FileOffset = uint64(sec.Offset) + owner.SectionOffset
		}
		*out = append(*out, owner)
	}
}

// sectionSymbolsAt returns the N_SECT defined symbols of section sectNo whose
// value lies in sec and is the greatest not exceeding pc. Aliases at that
// address are all returned. Only the current module's own symbol table is
// considered.
func (f *File) sectionSymbolsAt(sectNo uint8, sec *Section, pc uint64) []Symbol {
	if f.Symtab == nil {
		return nil
	}
	var (
		found bool
		best  uint64
		out   []Symbol
	)
	for _, sym := range f.Symtab.Syms {
		if sym.Type&NTypeMask != N_SECT || sym.Sect != sectNo {
			continue
		}
		if sym.Value > pc || sym.Value < sec.Addr || sym.Value >= sec.Addr+sec.Size {
			continue
		}
		switch {
		case !found || sym.Value > best:
			found = true
			best = sym.Value
			out = []Symbol{sym}
		case sym.Value == best:
			out = append(out, sym)
		}
	}
	return out
}

func isZerofillSection(flags uint32) bool {
	switch flags & SectionTypeMask {
	case SectionTypeZerofill, SectionTypeGBZerofill:
		return true
	}
	return false
}
