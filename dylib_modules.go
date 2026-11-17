package macho

// Dylib module, table-of-contents and external-reference relationships read
// from LC_DYSYMTAB. Everything here is derived read-only data: the image is
// never loaded, relocated or executed, and the Objective-C module addresses
// are recorded without dereferencing them.
//
// Index domains and units:
//   - DylibTableOfContents.SymbolIndex and every module Iextdefsym/Ilocalsym
//     window are absolute, zero-based indices into the LC_SYMTAB nlist array
//     (one entry per f.Symtab.Syms element), not offsets and not relative to
//     the module.
//   - DylibTableOfContents.ModuleIndex and DylibModule.Index are zero-based
//     positions in the module table.
//   - Module Irefsym/Nrefsym windows index Dysymtab.ExternalRefs (one entry
//     per packed 32-bit word); DylibExternalReference.SymbolIndex then names
//     the absolute nlist slot.
//   - DylibModule64.ModuleName and the packed init/terminator fields are raw
//     string-table offsets / section indices copied by meaning.
//   - ObjcModuleInfoAddr is a virtual address (32-bit values widened to
//     uint64) and ObjcModuleInfoSize is a byte count; neither is dereferenced.

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"github.com/blacktop/go-macho/internal/saferio"
	"github.com/blacktop/go-macho/types"
)

// DylibExternalReference is one 32-bit word of the LC_DYSYMTAB referenced
// symbol table (Extrefsymoff). The packed layout is isym:24 plus flags:8,
// but which end holds isym depends on the file byte order.
type DylibExternalReference struct {
	// Raw is the reference word exactly as read in the file's byte order.
	Raw uint32
	// SymbolIndex is the absolute LC_SYMTAB nlist index decoded from the
	// 24-bit isym field: low 24 bits in little-endian files, high 24 bits in
	// big-endian files.
	SymbolIndex uint32
	// Flags is the 8-bit reference flags: high 8 bits in little-endian files,
	// low 8 bits in big-endian files.
	Flags uint8
	// Symbol points at the referenced f.Symtab.Syms slot, or nil when the
	// table is resolved without an LC_SYMTAB (which NewFile rejects).
	Symbol *Symbol
}

// DylibModule is one module table entry plus the symbol relationships its
// windows describe. Same-named modules stay distinct entries and every window
// preserves physical order.
type DylibModule struct {
	// Index is the zero-based physical position of the module in the table.
	Index uint32
	// Name is the NUL-terminated string reached through Header.ModuleName in
	// the LC_SYMTAB string table, including names no nlist entry references.
	Name string
	// Header is the common widened representation. 32-bit files decode the
	// tail as Objective-C address (uint32) followed by size, and widen the
	// address; 64-bit files use the distinct size-then-uint64-address layout.
	// The address is stored but never dereferenced.
	Header types.DylibModule64
	// ExternalDefinitions are the nlist entries in the module's
	// Iextdefsym/Nextdefsym window; each pointer is the corresponding
	// f.Symtab.Syms slot.
	ExternalDefinitions []*Symbol
	// LocalSymbols are the nlist entries in the module's
	// Ilocalsym/Nlocalsym window; each pointer is the corresponding
	// f.Symtab.Syms slot.
	LocalSymbols []*Symbol
	// References are the external-reference words in the module's
	// Irefsym/Nrefsym window into Dysymtab.ExternalRefs, retained in order
	// with duplicates preserved.
	References []DylibExternalReference
	// TOCSymbols are the symbols named by the original table-of-contents
	// rows whose ModuleIndex selects this module, in TOC row order with
	// duplicate rows retained. Each pointer is the f.Symtab.Syms slot named
	// by the row's SymbolIndex.
	TOCSymbols []*Symbol
}

// parseDylibTables reads the table of contents, module table and referenced
// symbol table named by hdr and resolves every relationship against the
// already-parsed ordinary symbol table. The three tables are only consumed
// here, after both LC_SYMTAB and LC_DYSYMTAB have been collected, so the load
// commands may appear in either order. A zero table count suppresses every
// read at that table's recorded offset. Any truncated table, bad window,
// unterminated module name or index that does not link makes NewFile fail.
func (f *File) parseDylibTables(st *Dysymtab, nameAt func(uint64) string, strsize uint32, cmdOffset int64) error {
	hdr := &st.DysymtabCmd
	bo := f.ByteOrder
	nsyms := uint32(len(f.Symtab.Syms))

	// Undefined-symbol window: keep the historical bounds checks even when
	// LC_DYSYMTAB was collected before LC_SYMTAB, and extend them to the
	// other two global nlist windows the module windows must live inside.
	if err := checkNlistWindow("local symbols", hdr.Ilocalsym, hdr.Nlocalsym, nsyms, cmdOffset); err != nil {
		return err
	}
	if err := checkNlistWindow("externally defined symbols", hdr.Iextdefsym, hdr.Nextdefsym, nsyms, cmdOffset); err != nil {
		return err
	}
	if err := checkNlistWindow("undefined symbols", hdr.Iundefsym, hdr.Nundefsym, nsyms, cmdOffset); err != nil {
		return err
	}

	// External reference table (Nextrefsyms packed words).
	var refs []DylibExternalReference
	if hdr.Nextrefsyms > 0 {
		dat, err := saferio.ReadDataAt(f.cr, uint64(hdr.Nextrefsyms)*4, int64(hdr.Extrefsymoff))
		if err != nil {
			return fmt.Errorf("failed to read data at Extrefsymoff=%#x: %w", hdr.Extrefsymoff, err)
		}
		refs = make([]DylibExternalReference, hdr.Nextrefsyms)
		b := bytes.NewReader(dat)
		for i := range refs {
			var raw uint32
			if err := binary.Read(b, bo, &raw); err != nil {
				return fmt.Errorf("failed to read external reference %d: %w", i, err)
			}
			isym, flags := decodeDylibReference(raw, bo)
			if isym >= nsyms {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"external reference %d symbol index %d is outside the symbol table length %d",
					i, isym, nsyms), nil}
			}
			refs[i] = DylibExternalReference{
				Raw:         raw,
				SymbolIndex: isym,
				Flags:       flags,
				Symbol:      &f.Symtab.Syms[isym],
			}
		}
	}

	// Table of contents (Ntoc pairs of absolute nlist index / module index).
	var toc []types.DylibTableOfContents
	if hdr.Ntoc > 0 {
		dat, err := saferio.ReadDataAt(f.cr, uint64(hdr.Ntoc)*8, int64(hdr.Tocoffset))
		if err != nil {
			return fmt.Errorf("failed to read data at Tocoffset=%#x: %w", hdr.Tocoffset, err)
		}
		toc = make([]types.DylibTableOfContents, hdr.Ntoc)
		if err := binary.Read(bytes.NewReader(dat), bo, &toc); err != nil {
			return fmt.Errorf("failed to read table of contents: %w", err)
		}
		for i, row := range toc {
			if row.SymbolIndex >= nsyms {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"table of contents row %d symbol index %d is outside the symbol table length %d",
					i, row.SymbolIndex, nsyms), nil}
			}
			if row.ModuleIndex >= hdr.Nmodtab {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"table of contents row %d module index %d is outside the module table length %d",
					i, row.ModuleIndex, hdr.Nmodtab), nil}
			}
		}
	}

	// Module table (Nmodtab physical entries).
	var modules []DylibModule
	if hdr.Nmodtab > 0 {
		recSize := uint64(binary.Size(types.DylibModule64{}))
		if f.Magic != types.Magic64 {
			recSize = uint64(binary.Size(types.DylibModule{}))
		}
		dat, err := saferio.ReadDataAt(f.cr, uint64(hdr.Nmodtab)*recSize, int64(hdr.Modtaboff))
		if err != nil {
			return fmt.Errorf("failed to read data at Modtaboff=%#x: %w", hdr.Modtaboff, err)
		}
		modules = make([]DylibModule, hdr.Nmodtab)
		b := bytes.NewReader(dat)
		for i := range modules {
			var head types.DylibModule64
			if f.Magic == types.Magic64 {
				if err := binary.Read(b, bo, &head); err != nil {
					return fmt.Errorf("failed to read 64-bit module table entry %d: %w", i, err)
				}
			} else {
				var m32 types.DylibModule
				if err := binary.Read(b, bo, &m32); err != nil {
					return fmt.Errorf("failed to read module table entry %d: %w", i, err)
				}
				// The 32-bit tail is address then size; the widened header
				// keeps the size's meaning and zero-extends the address.
				head = types.DylibModule64{
					ModuleName:         m32.ModuleName,
					Iextdefsym:         m32.Iextdefsym,
					Nextdefsym:         m32.Nextdefsym,
					Irefsym:            m32.Irefsym,
					Nrefsym:            m32.Nrefsym,
					Ilocalsym:          m32.Ilocalsym,
					Nlocalsym:          m32.Nlocalsym,
					Iextrel:            m32.Iextrel,
					Nextrel:            m32.Nextrel,
					IinitIterm:         m32.IinitIterm,
					NinitNterm:         m32.NinitNterm,
					ObjcModuleInfoSize: m32.ObjcModuleInfoSize,
					ObjcModuleInfoAddr: uint64(m32.ObjcModuleInfoAddr),
				}
			}
			name, err := moduleName(nameAt, strsize, head.ModuleName)
			if err != nil {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"module %d name at string table offset %#x: %v", i, head.ModuleName, err), nil}
			}
			modules[i] = DylibModule{
				Index:  uint32(i),
				Name:   name,
				Header: head,
			}
		}

		// Resolve the per-module windows now that every header is decoded.
		for i := range modules {
			m := &modules[i]
			h := &m.Header
			if err := checkNlistWindow(fmt.Sprintf("module %d external definitions", i),
				h.Iextdefsym, h.Nextdefsym, nsyms, cmdOffset); err != nil {
				return err
			}
			if err := checkNlistWindow(fmt.Sprintf("module %d local symbols", i),
				h.Ilocalsym, h.Nlocalsym, nsyms, cmdOffset); err != nil {
				return err
			}
			// Module nlist windows must sit inside the matching global windows.
			if !windowInside(h.Iextdefsym, h.Nextdefsym, hdr.Iextdefsym, hdr.Nextdefsym) {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"module %d external definition window [%d,+%d) is outside the global window [%d,+%d)",
					i, h.Iextdefsym, h.Nextdefsym, hdr.Iextdefsym, hdr.Nextdefsym), nil}
			}
			if !windowInside(h.Ilocalsym, h.Nlocalsym, hdr.Ilocalsym, hdr.Nlocalsym) {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"module %d local symbol window [%d,+%d) is outside the global window [%d,+%d)",
					i, h.Ilocalsym, h.Nlocalsym, hdr.Ilocalsym, hdr.Nlocalsym), nil}
			}
			if err := checkRefWindow(fmt.Sprintf("module %d references", i),
				h.Irefsym, h.Nrefsym, hdr.Nextrefsyms, cmdOffset); err != nil {
				return err
			}
			m.ExternalDefinitions = symbolWindow(f.Symtab.Syms, h.Iextdefsym, h.Nextdefsym)
			m.LocalSymbols = symbolWindow(f.Symtab.Syms, h.Ilocalsym, h.Nlocalsym)
			m.References = append([]DylibExternalReference(nil), refs[h.Irefsym:h.Irefsym+h.Nrefsym]...)
		}

		// TOC rows link an absolute nlist slot to a module; the symbol must be
		// part of that module's external-definition window. Rows are not
		// assumed to be grouped, so each is appended in original order.
		for rowIdx, row := range toc {
			m := &modules[row.ModuleIndex]
			h := &m.Header
			if !windowInside(row.SymbolIndex, 1, h.Iextdefsym, h.Nextdefsym) {
				return &FormatError{cmdOffset, fmt.Sprintf(
					"table of contents row %d symbol index %d is not in module %d's external definition window [%d,+%d)",
					rowIdx, row.SymbolIndex, row.ModuleIndex, h.Iextdefsym, h.Nextdefsym), nil}
			}
			m.TOCSymbols = append(m.TOCSymbols, &f.Symtab.Syms[row.SymbolIndex])
		}
	} else if len(toc) > 0 {
		// A TOC naming modules requires a module table to name.
		return &FormatError{cmdOffset, "table of contents present but module table is empty", nil}
	}

	// Publish only once every table has been read and every relationship has
	// linked, so a failed NewFile never exposes a partially populated model.
	st.ExternalRefs = refs
	st.TOC = toc
	st.ModuleTable = modules
	return nil
}

// decodeDylibReference splits a reference word into the 24-bit nlist index
// and 8-bit flags. The Mach-O bit field is laid out in file byte order, so a
// little-endian word stores isym in its low 24 bits and a big-endian word
// stores isym in its high 24 bits. The fixed-shift helpers on
// types.DylibReference only model one layout and must not be applied to both.
func decodeDylibReference(raw uint32, bo binary.ByteOrder) (isym uint32, flags uint8) {
	if bo == binary.BigEndian {
		return raw >> 8, uint8(raw & 0xff)
	}
	return raw & 0x00ffffff, uint8(raw >> 24)
}

// checkNlistWindow verifies an absolute [start, start+count) window into the
// nlist table. A zero-length window is valid at any recorded start as long as
// the start itself does not exceed the table.
func checkNlistWindow(what string, start, count, length uint32, off int64) error {
	if start > length || start+count < start || start+count > length {
		return &FormatError{off, fmt.Sprintf(
			"%s window [%d,+%d) is outside the symbol table length %d",
			what, start, count, length), nil}
	}
	return nil
}

// checkRefWindow verifies a window into the external reference table.
func checkRefWindow(what string, start, count, length uint32, off int64) error {
	if start > length || start+count < start || start+count > length {
		return &FormatError{off, fmt.Sprintf(
			"%s window [%d,+%d) is outside the external reference table length %d",
			what, start, count, length), nil}
	}
	return nil
}

// windowInside reports whether the inner window is contained by the outer one.
func windowInside(innerStart, innerCount, outerStart, outerCount uint32) bool {
	if innerCount == 0 {
		return innerStart >= outerStart && innerStart <= outerStart+outerCount
	}
	return innerStart >= outerStart &&
		innerStart+innerCount <= outerStart+outerCount
}

// symbolWindow returns pointers to the nlist slots in [start, start+count).
// The pointers alias the original f.Symtab.Syms backing array rather than
// copies, so identity survives across every relationship.
func symbolWindow(syms []Symbol, start, count uint32) []*Symbol {
	out := make([]*Symbol, count)
	for i := range out {
		out[i] = &syms[start+uint32(i)]
	}
	return out
}

// moduleName resolves a module name through the LC_SYMTAB string table. The
// name may be referenced by no nlist entry, so it must be read directly here
// rather than inferred from a symbol name. nameAt honors the same table
// bounds as the ordinary symbol path; a name that runs to the end of the
// table without a NUL is rejected.
func moduleName(nameAt func(uint64) string, strsize, nameOff uint32) (string, error) {
	if nameOff >= strsize {
		return "", fmt.Errorf("offset %#x is outside the string table of size %#x",
			nameOff, strsize)
	}
	s := nameAt(uint64(nameOff))
	if uint64(len(s)) >= uint64(strsize)-uint64(nameOff) {
		return "", ErrCStringNoTerminator
	}
	return s, nil
}
