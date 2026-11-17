package net.fornwall.jelf;

/**
 * An ELF section holding relocation entries with explicit addends.
 *
 * <p>Instances are created for both {@link ElfSectionHeader#SHT_RELA} sections holding a plain
 * array of {@code Elf32_Rela}/{@code Elf64_Rela} records and for {@link
 * ElfSectionHeader#SHT_ANDROID_RELA} sections holding an {@code APS2} packed stream. In both cases
 * the expanded entries are available from {@link #relocations} in stream order. The section header
 * and {@link #getData()} always expose the original on-disk bytes.
 */
public final class ElfRelocationAddendSection extends ElfSection {
    public final ElfRelocationAddend[] relocations;

    ElfRelocationAddendSection(ElfParser parser, ElfSectionHeader header) {
        super(parser, header);

        if (header.sh_type == ElfSectionHeader.SHT_ANDROID_RELA) {
            String name = AndroidPackedRelocations.sectionName(header);
            byte[] data;
            try {
                data = getData();
            } catch (ElfException e) {
                throw new ElfException(
                        "Section '" + name + "' packed relocations at byte 0: could not read section data", e);
            }
            AndroidPackedRelocations.Result decoded =
                    AndroidPackedRelocations.decode(name, parser.elfFile.is32Bits() ? 32 : 64, data, true);
            relocations = new ElfRelocationAddend[decoded.offsets.length];
            for (int i = 0; i < relocations.length; i++) {
                relocations[i] = new ElfRelocationAddend(
                        parser.elfFile,
                        decoded.offsets[i],
                        decoded.infos[i],
                        decoded.addends[i],
                        header.sh_link);
            }
        } else {
            int num_entries = (int) (header.sh_size / header.sh_entsize);
            relocations = new ElfRelocationAddend[num_entries];
            for (int i = 0; i < num_entries; i++) {
                final long relOffset = header.sh_offset + (i * header.sh_entsize);
                relocations[i] = new ElfRelocationAddend(parser, relOffset, header.sh_link);
            }
        }
    }
}
