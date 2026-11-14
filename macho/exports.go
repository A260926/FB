package macho

const N_SECT = 0x0E
const N_PEXT = 0x10
const N_EXT = 0x01

// NTypeMask selects the symbol type bits (N_UNDF, N_ABS, N_SECT, N_PBUD)
// out of a Symbol's Type.
const NTypeMask = 0x0E

// Export - describes a single export entry (similar to PE version for future refactoring)
type Export struct {
	//Ordinal        uint32 // no ordinals for Mach-O
	Name           string
	VirtualAddress uint64
}

// Exports - gets exports, including private exports
func (f *File) Exports() []Export {
	var exports []Export
	for _, symbol := range f.Symtab.Syms {
		if (symbol.Type&N_PEXT == N_PEXT ||
			symbol.Type&N_EXT == N_EXT) && symbol.Value != 0 {
			var export Export
			export.Name = symbol.Name
			export.VirtualAddress = symbol.Value
			exports = append(exports, export)
		}
	}
	return exports
}
