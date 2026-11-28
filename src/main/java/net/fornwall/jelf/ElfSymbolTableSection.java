package net.fornwall.jelf;

/**
 * An ELF section with symbol information.
 *
 * This class represents either of two section types:
 * <ul>
 *     <li>{@link ElfSectionHeader#SHT_DYNSYM}: For a minimal set of symbols adequate for dynamic linking. Can be stripped and has no runtime cost (is non-allocable). Normally named ".dynsym".</li>
 *     <li>{@link ElfSectionHeader#SHT_SYMTAB}: A complete symbol table typically used for link editing. Can not be stripped (is allocable). Normally named ".symtab".</li>
 * </ul>
 */
public class ElfSymbolTableSection extends ElfSection {

    public final ElfSymbol[] symbols;

    ElfSymbolTableSection(ElfParser parser, ElfSectionHeader header) {
        super(parser, header);

        int num_entries = (int) (header.sh_size / header.sh_entsize);
        symbols = new ElfSymbol[num_entries];
        for (int i = 0; i < num_entries; i++) {
            final long symbolOffset = header.sh_offset + (i * header.sh_entsize);
            symbols[i] = new ElfSymbol(parser, symbolOffset, header.sh_type, this);
        }
    }

    /** The string table referenced by this symbol table's {@code sh_link}. */
    ElfStringTable getStringTable() {
        int link = header.sh_link;
        if (link < 0 || link >= parser.elfFile.e_shnum) {
            throw new ElfException("Symbol table section '" + header.getName() + "' sh_link=" + link
                    + " is outside the section header table");
        }
        ElfSection linked = parser.elfFile.getSection(link);
        if (!(linked instanceof ElfStringTable)) {
            throw new ElfException("Symbol table section '" + header.getName() + "' sh_link=" + link
                    + " does not reference a string table");
        }
        return (ElfStringTable) linked;
    }
}
