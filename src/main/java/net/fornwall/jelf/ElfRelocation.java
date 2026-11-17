package net.fornwall.jelf;

import java.util.Objects;

/**
 * A relocation connects a symbolic reference with its actual definition.
 * Relocatable files must have information that describes how to modify their
 * section contents, thus allowing executable and shared object files to hold
 * the right information for a process's program image. Relocation entries are
 * these data.
 * <p>
 * These elf relocation entries can be obtained from {@link ElfRelocationSection}:s.
 * <p>
 * Corresponds to the below C structs:
 * <pre><code>
 * typedef struct {
 *   Elf32_Addr r_offset;
 *   Elf32_Word r_info;
 * } Elf32_Rel;
 *
 * typedef struct {
 *   Elf64_Addr	r_offset;
 *   Elf64_Xword r_info;
 * } Elf64_Rel;
 * </code></pre>
 */
public final class ElfRelocation {
    /**
     * The location at which to apply the relocation. For a relocatable file,
     * this is the byte offset from the beginning of the section. For an
     * executable or shared object, the value is the virtual address.
     */
    public final long r_offset; // Elf32_Addr or Elf64_Addr

    /**
     * This member gives both the symbol table index to which the relocation
     * must be made, and the type of relocation to apply. Relocation types are
     * processor specific.
     */
    public final long r_info; // Elf32_Word or Elf64_Xword

    private final ElfFile elfFile;
    /** Section index of the symbol table referenced by the owning relocation section's sh_link. */
    private final int symbolTableSectionIndex;

    ElfRelocation(ElfParser parser, long offset, int symbolTableSectionIndex) {
        parser.seek(offset);

        r_offset = parser.readIntOrLong();
        r_info = parser.readIntOrLong();
        elfFile = parser.elfFile;
        this.symbolTableSectionIndex = symbolTableSectionIndex;
    }

    /** Creates a relocation already decoded from an Android packed relocation stream. */
    ElfRelocation(ElfFile elfFile, long r_offset, long r_info, int symbolTableSectionIndex) {
        this.elfFile = elfFile;
        this.r_offset = r_offset;
        this.r_info = r_info;
        this.symbolTableSectionIndex = symbolTableSectionIndex;
    }

    /**
     * Corresponds to the ELF32_R_TYPE / ELF64_R_TYPE macros.
     *
     * @see ElfRelocationTypes
     */
    public long getType() {
        return elfFile.is32Bits() ? (r_info & 0xFF) : ((int) r_info);
    }

    /**
     * The symbol table index, with respect to which the relocation must be made.
     * Use {@link #getSymbol()} to get the resolved {@link ElfSymbol} from this index.
     * <p>
     * Corresponds to the ELF32_R_SYM / ELF64_R_SYM macros.
     */
    public int getSymbolIndex() {
        return (int) (r_info >> (elfFile.is32Bits() ? 8 : 32));
    }

    /**
     * The symbol with respect to which the relocation must be made.
     * Use {@link #getSymbolIndex()}} to get the resolved {@link ElfSymbol} from this index.
     * <p>
     * The symbol is looked up in the symbol table referenced by the owning relocation
     * section's {@code sh_link}.
     */
    public ElfSymbol getSymbol() {
        ElfSymbolTableSection table = elfFile.symbolTableSectionAt(symbolTableSectionIndex);
        int symbolIndex = getSymbolIndex();
        if (symbolIndex < 0 || symbolIndex >= table.symbols.length) {
            throw new ElfException("Relocation symbol index " + symbolIndex + " is out of range for the symbol table"
                    + " at section " + symbolTableSectionIndex + " (" + table.symbols.length + " entries)");
        }
        return table.symbols[symbolIndex];
    }

    @Override
    public boolean equals(Object o) {
        if (o == null || getClass() != o.getClass()) {
            return false;
        }

        ElfRelocation that = (ElfRelocation) o;
        return r_offset == that.r_offset && r_info == that.r_info;
    }

    @Override
    public int hashCode() {
        return Objects.hash(r_offset, r_info);
    }
}
