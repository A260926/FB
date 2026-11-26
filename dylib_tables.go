package macho

// LC_DYSYMTAB dylib tables: table of contents, module table and the referenced
// symbol table. These tables exist in dynamically linked shared libraries and
// are resolved purely from file bytes: no dependent libraries are loaded and
// no Objective-C memory pointed at by a module header is read.
//
// Index domains used throughout:
//   - Dysymtab.TOC[].SymbolIndex and every DylibExternalReference.SymbolIndex
//     are absolute indexes into the LC_SYMTAB nlist array (File.Symtab.Syms);
//     they are not offsets.
//   - DylibTableOfContents.ModuleIndex and DylibModule.Index are zero-based
//     indexes into the physical module table (Dysymtab.ModuleTable).
//   - DylibModule header Iextdefsym/Ilocalsym fields are absolute nlist
//     indexes; the matching N* fields are counts (number of entries).
//   - DylibModule header Irefsym is an index into Dysymtab.ExternalRefs and
//     Nrefsym is the number of reference entries in that window.
//   - Header ObjcModuleInfoAddr is a virtual address (widened to uint64 for
//     32-bit images) and ObjcModuleInfoSize is a size in bytes; neither is
//     dereferenced here.

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/blacktop/go-macho/internal/saferio"
	"github.com/blacktop/go-macho/types"
)

// parseDylibTables reads the TOC, module table and external reference table
// described by st, then links every relationship against the already parsed
// f.Symtab. It runs only after both LC_SYMTAB and LC_DYSYMTAB have been
// collected, so the commands may appear in either order. A zero-count table is
// never read at its recorded offset. Any truncated read, invalid window,
// unterminated name or unresolvable index link fails NewFile rather than
// returning a partially populated model.
func (f *File) parseDylibTables(st *Dysymtab) error {
	hdr := st.DysymtabCmd
	nsyms := uint32(len(f.Symtab.Syms))

	// Validate the global nlist windows first; every per-module window must
	// fit inside one of them.
	if err := checkNlistWindow("local symbol", hdr.Ilocalsym, hdr.Nlocalsym, nsyms); err != nil {
		return err
	}
	if err := checkNlistWindow("external definition", hdr.Iextdefsym, hdr.Nextdefsym, nsyms); err != nil {
		return err
	}

	refs, err := f.parseExternalRefs(hdr, nsyms)
	if err != nil {
		return err
	}
	st.ExternalRefs = refs

	toc, err := f.parseTOC(hdr)
	if err != nil {
		return err
	}
	st.TOC = toc

	headers, err := f.parseModuleHeaders(hdr)
	if err != nil {
		return err
	}

	var modules []DylibModule
	if len(headers) > 0 {
		modules = make([]DylibModule, len(headers))
	}
	for i, h := range headers {
		name, err := f.moduleName(h.ModuleName)
		if err != nil {
			return err
		}
		m := DylibModule{
			Index:  uint32(i),
			Name:   name,
			Header: h,
		}
		if err := checkNlistWindow("module external definition", h.Iextdefsym, h.Nextdefsym, nsyms); err != nil {
			return err
		}
		if err := checkSubWindow("module external definition", h.Iextdefsym, h.Nextdefsym, hdr.Iextdefsym, hdr.Nextdefsym); err != nil {
			return err
		}
		m.ExternalDefinitions = f.symbolPointers(h.Iextdefsym, h.Nextdefsym)

		if err := checkNlistWindow("module local symbol", h.Ilocalsym, h.Nlocalsym, nsyms); err != nil {
			return err
		}
		if err := checkSubWindow("module local symbol", h.Ilocalsym, h.Nlocalsym, hdr.Ilocalsym, hdr.Nlocalsym); err != nil {
			return err
		}
		m.LocalSymbols = f.symbolPointers(h.Ilocalsym, h.Nlocalsym)

		if err := checkRefWindow(h.Irefsym, h.Nrefsym, uint32(len(refs))); err != nil {
			return err
		}
		// Slicing preserves file order, including repeated entries, and keeps
		// the same DylibExternalReference values (with their Symbol pointers)
		// as the top-level table.
		m.References = refs[h.Irefsym : h.Irefsym+h.Nrefsym]

		modules[i] = m
	}

	// Resolve TOC rows last: each row links the global nlist to a module, and
	// its symbol must belong to that module's external-definition window. Rows
	// are not assumed to be grouped by module.
	tocSymbols := make([][]*Symbol, len(modules))
	for i, row := range toc {
		rowOff := int64(hdr.Tocoffset) + int64(i)*8
		if row.ModuleIndex >= uint32(len(modules)) {
			return &FormatError{rowOff, fmt.Sprintf(
				"table of contents entry %d module index %d is out of range (module table has %d entries)",
				i, row.ModuleIndex, len(modules)), nil}
		}
		if row.SymbolIndex >= nsyms {
			return &FormatError{rowOff, fmt.Sprintf(
				"table of contents entry %d symbol index %d is out of range (symbol table has %d entries)",
				i, row.SymbolIndex, nsyms), nil}
		}
		m := modules[row.ModuleIndex]
		extEnd := m.Header.Iextdefsym + m.Header.Nextdefsym
		if row.SymbolIndex < m.Header.Iextdefsym || row.SymbolIndex >= extEnd {
			return &FormatError{rowOff, fmt.Sprintf(
				"table of contents entry %d symbol index %d is outside module %d external definition window [%d,%d)",
				i, row.SymbolIndex, row.ModuleIndex, m.Header.Iextdefsym, extEnd), nil}
		}
		tocSymbols[row.ModuleIndex] = append(tocSymbols[row.ModuleIndex], &f.Symtab.Syms[row.SymbolIndex])
	}
	for i := range modules {
		modules[i].TOCSymbols = tocSymbols[i]
	}
	st.ModuleTable = modules
	return nil
}

// parseExternalRefs reads the referenced symbol table, decoding each 24-bit
// nlist index / 8-bit flags word according to the file byte order. Raw keeps
// the word read in file order; the fixed-shift types.DylibReference helpers
// describe the big-endian bit layout and are not applied to little-endian
// inputs.
func (f *File) parseExternalRefs(hdr types.DysymtabCmd, nsyms uint32) ([]DylibExternalReference, error) {
	if hdr.Nextrefsyms == 0 {
		return nil, nil
	}
	dat, err := saferio.ReadDataAt(f.cr, uint64(hdr.Nextrefsyms)*4, int64(hdr.Extrefsymoff))
	if err != nil {
		return nil, fmt.Errorf("failed to read data at Extrefsymoff @ %#x: %w", int64(hdr.Extrefsymoff), err)
	}
	refs := make([]DylibExternalReference, hdr.Nextrefsyms)
	rd := bytes.NewReader(dat)
	for i := range refs {
		var raw uint32
		if err := binary.Read(rd, f.ByteOrder, &raw); err != nil {
			return nil, fmt.Errorf("failed to read dylib external reference %d: %w", i, err)
		}
		var isym uint32
		var flags uint8
		switch f.ByteOrder {
		case binary.LittleEndian:
			isym = raw & 0x00ffffff
			flags = uint8(raw >> 24)
		case binary.BigEndian:
			isym = raw >> 8
			flags = uint8(raw & 0xff)
		default:
			panic("unreachable")
		}
		if isym >= nsyms {
			return nil, &FormatError{
				int64(hdr.Extrefsymoff) + int64(i)*4,
				fmt.Sprintf("dylib external reference %d symbol index %d is out of range (symbol table has %d entries)", i, isym, nsyms),
				nil,
			}
		}
		refs[i] = DylibExternalReference{
			Raw:         raw,
			SymbolIndex: isym,
			Flags:       flags,
			Symbol:      &f.Symtab.Syms[isym],
		}
	}
	return refs, nil
}

// parseTOC reads the table of contents rows verbatim, in file order. Repeated
// rows are retained.
func (f *File) parseTOC(hdr types.DysymtabCmd) ([]types.DylibTableOfContents, error) {
	if hdr.Ntoc == 0 {
		return nil, nil
	}
	dat, err := saferio.ReadDataAt(f.cr, uint64(hdr.Ntoc)*8, int64(hdr.Tocoffset))
	if err != nil {
		return nil, fmt.Errorf("failed to read data at Tocoffset @ %#x: %w", int64(hdr.Tocoffset), err)
	}
	toc := make([]types.DylibTableOfContents, hdr.Ntoc)
	if err := binary.Read(bytes.NewReader(dat), f.ByteOrder, &toc); err != nil {
		return nil, fmt.Errorf("failed to read dylib table of contents: %w", err)
	}
	return toc, nil
}

// parseModuleHeaders reads the physical module table and returns each entry in
// the widened types.DylibModule64 representation. The 32-bit tail is
// (ObjcModuleInfoAddr uint32, ObjcModuleInfoSize uint32) while the 64-bit tail
// is (ObjcModuleInfoSize uint32, ObjcModuleInfoAddr uint64); the fields are
// mapped by meaning rather than by position.
func (f *File) parseModuleHeaders(hdr types.DysymtabCmd) ([]types.DylibModule64, error) {
	if hdr.Nmodtab == 0 {
		return nil, nil
	}
	wide := f.Magic == types.Magic64
	modSize := binary.Size(types.DylibModule{})
	if wide {
		modSize = binary.Size(types.DylibModule64{})
	}
	dat, err := saferio.ReadDataAt(f.cr, uint64(hdr.Nmodtab)*uint64(modSize), int64(hdr.Modtaboff))
	if err != nil {
		return nil, fmt.Errorf("failed to read data at Modtaboff @ %#x: %w", int64(hdr.Modtaboff), err)
	}
	if wide {
		modules := make([]types.DylibModule64, hdr.Nmodtab)
		if err := binary.Read(bytes.NewReader(dat), f.ByteOrder, &modules); err != nil {
			return nil, fmt.Errorf("failed to read 64-bit dylib module table: %w", err)
		}
		return modules, nil
	}
	narrow := make([]types.DylibModule, hdr.Nmodtab)
	if err := binary.Read(bytes.NewReader(dat), f.ByteOrder, &narrow); err != nil {
		return nil, fmt.Errorf("failed to read 32-bit dylib module table: %w", err)
	}
	modules := make([]types.DylibModule64, len(narrow))
	for i, m := range narrow {
		modules[i] = widenModule32(m)
	}
	return modules, nil
}

// widenModule32 converts a physical 32-bit module entry to the common widened
// header, widening ObjcModuleInfoAddr and keeping ObjcModuleInfoSize's meaning.
func widenModule32(m types.DylibModule) types.DylibModule64 {
	return types.DylibModule64{
		ModuleName:         m.ModuleName,
		Iextdefsym:         m.Iextdefsym,
		Nextdefsym:         m.Nextdefsym,
		Irefsym:            m.Irefsym,
		Nrefsym:            m.Nrefsym,
		Ilocalsym:          m.Ilocalsym,
		Nlocalsym:          m.Nlocalsym,
		Iextrel:            m.Iextrel,
		Nextrel:            m.Nextrel,
		IinitIterm:         m.IinitIterm,
		NinitNterm:         m.NinitNterm,
		ObjcModuleInfoSize: m.ObjcModuleInfoSize,
		ObjcModuleInfoAddr: uint64(m.ObjcModuleInfoAddr),
	}
}

// moduleName resolves a module name through the retained LC_SYMTAB string
// table lookup, applying the same Strsize bounds as ordinary symbol names and
// requiring a NUL terminator inside the table. Module names are never inferred
// from nlist names.
func (f *File) moduleName(off uint32) (string, error) {
	if f.symtabNameAt == nil || uint64(off) >= f.symtabStrsize {
		return "", fmt.Errorf("module name string table offset %#x is out of range (Strsize=%#x)", off, f.symtabStrsize)
	}
	s := f.symtabNameAt(uint64(off))
	if uint64(len(s)) >= f.symtabStrsize-uint64(off) {
		return "", &FormatError{
			int64(f.Symtab.Stroff) + int64(off),
			fmt.Sprintf("module name at string table offset %#x has no NUL terminator before Strsize=%#x", off, f.symtabStrsize),
			nil,
		}
	}
	return s, nil
}

// symbolPointers returns pointers to the nlist slots in [start, start+count),
// always addressing the backing File.Symtab.Syms elements directly.
func (f *File) symbolPointers(start, count uint32) []*Symbol {
	out := make([]*Symbol, count)
	for i := range out {
		out[i] = &f.Symtab.Syms[start+uint32(i)]
	}
	return out
}

// checkNlistWindow verifies that [start, start+count) is a valid window into
// an nlist array of limit entries, guarding against count overflow.
func checkNlistWindow(what string, start, count, limit uint32) error {
	if start > limit || start+count < start || start+count > limit {
		return fmt.Errorf(
			"invalid %s window [%d,+%d) for symbol table of %d entries", what, start, count, limit)
	}
	return nil
}

// checkSubWindow verifies that [start, start+count) lies inside the global
// window [gstart, gstart+gcount). Both windows must already have been checked
// for overflow.
func checkSubWindow(what string, start, count, gstart, gcount uint32) error {
	end := start + count
	gend := gstart + gcount
	if start < gstart || end > gend {
		return fmt.Errorf(
			"invalid %s window [%d,+%d) outside dynamic symbol table window [%d,+%d)",
			what, start, count, gstart, gcount)
	}
	return nil
}

// checkRefWindow verifies that [start, start+count) is a valid window into the
// external reference table.
func checkRefWindow(start, count, limit uint32) error {
	if start > limit || start+count < start || start+count > limit {
		return fmt.Errorf(
			"invalid module reference window [%d,+%d) for external reference table of %d entries",
			start, count, limit)
	}
	return nil
}
