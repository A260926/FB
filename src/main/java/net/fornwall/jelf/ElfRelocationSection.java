package net.fornwall.jelf;

import java.util.List;

public final class ElfRelocationSection extends ElfSection {
    public final ElfRelocation[] relocations;

    ElfRelocationSection(ElfParser parser, ElfSectionHeader header) {
        super(parser, header);

        if (header.sh_type == ElfSectionHeader.SHT_ANDROID_REL) {
            relocations = unpack(parser);
            return;
        }

        int num_entries = (int) (header.sh_size / header.sh_entsize);
        relocations = new ElfRelocation[num_entries];
        for (int i = 0; i < num_entries; i++) {
            final long relOffset = header.sh_offset + (i * header.sh_entsize);
            relocations[i] = new ElfRelocation(parser, relOffset, this);
        }
    }

    private ElfRelocation[] unpack(ElfParser parser) {
        // getData() keeps the on-disk APS2 bytes; only the expanded view below is interpreted.
        List<AndroidPackedRelocations.Entry> entries =
                AndroidPackedRelocations.decode(getData(), header, !parser.elfFile.is32Bits());
        ElfRelocation[] result = new ElfRelocation[entries.size()];
        for (int i = 0; i < result.length; i++) {
            AndroidPackedRelocations.Entry entry = entries.get(i);
            result[i] = new ElfRelocation(entry.offset, entry.info, parser.elfFile, this);
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
