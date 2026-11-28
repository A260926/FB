package net.fornwall.jelf;

import java.util.List;

public final class ElfRelocationAddendSection extends ElfSection {
    public final ElfRelocationAddend[] relocations;

    ElfRelocationAddendSection(ElfParser parser, ElfSectionHeader header) {
        super(parser, header);

        if (header.sh_type == ElfSectionHeader.SHT_ANDROID_RELA) {
            relocations = unpack(parser);
            return;
        }

        int num_entries = (int) (header.sh_size / header.sh_entsize);
        relocations = new ElfRelocationAddend[num_entries];
        for (int i = 0; i < num_entries; i++) {
            final long relOffset = header.sh_offset + (i * header.sh_entsize);
            relocations[i] = new ElfRelocationAddend(parser, relOffset, this);
        }
    }

    private ElfRelocationAddend[] unpack(ElfParser parser) {
        // getData() keeps the on-disk APS2 bytes; only the expanded view below is interpreted.
        List<AndroidPackedRelocations.Entry> entries =
                AndroidPackedRelocations.decode(getData(), header, !parser.elfFile.is32Bits());
        ElfRelocationAddend[] result = new ElfRelocationAddend[entries.size()];
        for (int i = 0; i < result.length; i++) {
            AndroidPackedRelocations.Entry entry = entries.get(i);
            result[i] = new ElfRelocationAddend(entry.offset, entry.info, entry.addend, parser.elfFile, this);
        }
        return result;
    }

    /** The symbol table referenced by this section's {@code sh_link}. */
    ElfSymbolTableSection getSymbolTable() {
        int link = header.sh_link;
        if (link < 0 || link >= parser.elfFile.e_shnum) {
            throw new ElfException("Relocation section '" + header.getName() + "' sh_link=" + link
                    + " is outside the section header table");
        }
        ElfSection linked = parser.elfFile.getSection(link);
        if (!(linked instanceof ElfSymbolTableSection)) {
            throw new ElfException("Relocation section '" + header.getName() + "' sh_link=" + link
                    + " does not reference a symbol table");
        }
        return (ElfSymbolTableSection) linked;
    }
}
