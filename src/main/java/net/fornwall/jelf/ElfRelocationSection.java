package net.fornwall.jelf;

/**
 * An ELF section holding relocation entries without explicit addends.
 *
 * <p>Instances are created for both {@link ElfSectionHeader#SHT_REL} sections holding a plain
 * array of {@code Elf32_Rel}/{@code Elf64_Rel} records and for {@link
 * ElfSectionHeader#SHT_ANDROID_REL} sections holding an {@code APS2} packed stream. In both cases
 * the expanded entries are available from {@link #relocations} in stream order. The section header
 * and {@link #getData()} always expose the original on-disk bytes.
 */
public final class ElfRelocationSection extends ElfSection {
    public final ElfRelocation[] relocations;

    ElfRelocationSection(ElfParser parser, ElfSectionHeader header) {
        super(parser, header);

        if (header.sh_type == ElfSectionHeader.SHT_ANDROID_REL) {
            String name = AndroidPackedRelocations.sectionName(header);
            byte[] data;
            try {
                data = getData();
            } catch (ElfException e) {
                throw new ElfException(
                        "Section '" + name + "' packed relocations at byte 0: could not read section data", e);
            }
            AndroidPackedRelocations.Result decoded =
                    AndroidPackedRelocations.decode(name, parser.elfFile.is32Bits() ? 32 : 64, data, false);
            relocations = new ElfRelocation[decoded.offsets.length];
            for (int i = 0; i < relocations.length; i++) {
                relocations[i] = new ElfRelocation(
                        parser.elfFile, decoded.offsets[i], decoded.infos[i], header.sh_link);
            }
        } else {
            int num_entries = (int) (header.sh_size / header.sh_entsize);
            relocations = new ElfRelocation[num_entries];
            for (int i = 0; i < num_entries; i++) {
                final long relOffset = header.sh_offset + (i * header.sh_entsize);
                relocations[i] = new ElfRelocation(parser, relOffset, header.sh_link);
            }
        }
    }
}
