The public model is populated by macho.NewFile; it is not an optional second parser.

In package macho, extend Dysymtab with:

```go
TOC         []types.DylibTableOfContents
ModuleTable []DylibModule
ExternalRefs []DylibExternalReference
```

The following public shapes define the read-only relationships:

```go
type DylibExternalReference struct {
    Raw uint32
    SymbolIndex uint32
    Flags uint8
    Symbol *Symbol
}
type DylibModule struct {
    Index uint32
    Name string
    Header types.DylibModule64
    ExternalDefinitions []*Symbol
    LocalSymbols []*Symbol
    References []DylibExternalReference
    TOCSymbols []*Symbol
}
```

ModuleTable preserves original module order, and Index is that zero-based index.
Name uses Header.ModuleName in the LC_SYMTAB string table, including names not
referenced by any nlist. Header is the common widened representation: 32-bit
ObjcModuleInfoAddr is widened and ObjcModuleInfoSize keeps its original meaning;
64-bit files use the distinct 64-bit tail layout. Other fields are copied by
meaning, including the packed initializer/terminator fields, without interpreting
those routines or reading Objective-C memory.

Module Iextdefsym/Nextdefsym and Ilocalsym/Nlocalsym are absolute nlist windows,
inside the corresponding global LC_DYSYMTAB windows. Irefsym/Nrefsym is a window
into ExternalRefs. TOC SymbolIndex is an absolute nlist index, ModuleIndex is a
module index, and its symbol must be in that module's external-definition window.
TOCSymbols is the subsequence of original TOC rows for that module. Repeated rows
and references are retained, not deduplicated. All Symbol pointers must equal the
address of the corresponding f.Symtab.Syms element; names are not identities.

Read 32-bit reference words using the file byte order. In little-endian Mach-O,
isym occupies low 24 bits and flags high 8 bits; in big-endian Mach-O, isym is
high 24 bits and flags low 8 bits. Raw is the numeric word read in file order.
Keep the existing types.DylibReference helpers compatible; the new model must not
blindly apply their fixed shift to both file byte orders.

Tables are resolved after both load commands are known, in either command order.
Use the existing ReaderAt, file byte order and normal error mechanism. Nonempty
tables need complete readable bytes, every range and name must be valid, and
invalid links return an error from NewFile rather than a partially usable model.
A zero table count does not cause a read at its recorded file offset. Missing
LC_DYSYMTAB leaves the ordinary symbol path unchanged. Existing global undefined
symbol window checks remain effective even when command order is reversed.
IndirectSyms and source bytes retain their previous meanings. This task does not
extend Export to serialize the new relationships.

Fixture.Build constructs normal 32/64-bit, little/big-endian inputs with no
executable instructions. Its offset metadata identifies physical input tables,
not a parsed solution. Extend it for focused consistency checks as needed.
