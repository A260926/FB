// MIT License

// Copyright (c) 2018 finixbit

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "elf_parser.hpp"
#include <cstring>
#include <stdexcept>
#include <unistd.h> /* close */
using namespace elf_parser;

namespace {

// Decoded file header and section headers. Everything is read byte by byte
// from the mapped image so that neither host endianness nor host word size
// affects the result; the mapping itself is never modified.

struct raw_ehdr {
    uint8_t  elf_class;
    uint8_t  data_encoding;
    uint16_t type;
    uint16_t machine;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct raw_shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

struct raw_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct raw_sym {
    uint32_t name;
    uint8_t  info;
    uint8_t  other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(std::string("elf_parser: ") + message);
}

uint64_t read_u(const uint8_t *base, size_t file_size, size_t off, unsigned n) {
    require(off <= file_size && n <= file_size - off, "read past end of file");
    uint64_t value = 0;
    // The byte order to interpret is taken from the file itself, never from
    // the host: ELFDATA2LSB stores the least significant byte first.
    if (base[EI_DATA] == ELFDATA2LSB) {
        for (unsigned i = 0; i < n; ++i)
            value |= uint64_t(base[off + i]) << (8 * i);
    } else {
        for (unsigned i = 0; i < n; ++i)
            value = (value << 8) | base[off + i];
    }
    return value;
}

int64_t sign_extend(uint64_t value, unsigned bits) {
    uint64_t sign = uint64_t(1) << (bits - 1);
    return int64_t((value ^ sign) - sign);
}

const char *str_at(const uint8_t *base, size_t file_size, size_t off) {
    require(off < file_size, "string table index past end of file");
    return reinterpret_cast<const char *>(base + off);
}

raw_ehdr read_ehdr(const uint8_t *base, size_t file_size) {
    require(file_size >= EI_NIDENT, "file too small for ELF identification");
    require(base[EI_MAG0] == ELFMAG0 && base[EI_MAG1] == ELFMAG1 &&
            base[EI_MAG2] == ELFMAG2 && base[EI_MAG3] == ELFMAG3,
            "bad ELF magic");
    require(base[EI_CLASS] == ELFCLASS32 || base[EI_CLASS] == ELFCLASS64,
            "unsupported ELF class");
    require(base[EI_DATA] == ELFDATA2LSB || base[EI_DATA] == ELFDATA2MSB,
            "unsupported ELF data encoding");

    raw_ehdr h;
    h.elf_class = base[EI_CLASS];
    h.data_encoding = base[EI_DATA];
    const bool is64 = (h.elf_class == ELFCLASS64);
    require(file_size >= (is64 ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr)),
            "file too small for ELF header");

    h.type    = uint16_t(read_u(base, file_size, 16, 2));
    h.machine = uint16_t(read_u(base, file_size, 18, 2));
    require(h.machine == EM_386 || h.machine == EM_PPC ||
            h.machine == EM_X86_64 || h.machine == EM_PPC64,
            "unsupported e_machine");

    if (is64) {
        h.entry     = read_u(base, file_size, 24, 8);
        h.phoff     = read_u(base, file_size, 32, 8);
        h.shoff     = read_u(base, file_size, 40, 8);
        h.phentsize = uint16_t(read_u(base, file_size, 54, 2));
        h.phnum     = uint16_t(read_u(base, file_size, 56, 2));
        h.shentsize = uint16_t(read_u(base, file_size, 58, 2));
        h.shnum     = uint16_t(read_u(base, file_size, 60, 2));
        h.shstrndx  = uint16_t(read_u(base, file_size, 62, 2));
    } else {
        h.entry     = read_u(base, file_size, 24, 4);
        h.phoff     = read_u(base, file_size, 28, 4);
        h.shoff     = read_u(base, file_size, 32, 4);
        h.phentsize = uint16_t(read_u(base, file_size, 42, 2));
        h.phnum     = uint16_t(read_u(base, file_size, 44, 2));
        h.shentsize = uint16_t(read_u(base, file_size, 46, 2));
        h.shnum     = uint16_t(read_u(base, file_size, 48, 2));
        h.shstrndx  = uint16_t(read_u(base, file_size, 50, 2));
    }
    return h;
}

std::vector<raw_shdr> read_shdrs(const uint8_t *base, size_t file_size,
                                 const raw_ehdr &h) {
    std::vector<raw_shdr> out;
    const bool is64 = (h.elf_class == ELFCLASS64);
    const size_t entsize = is64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);
    if (h.shnum == 0 || h.shoff == 0)
        return out;
    require(h.shoff <= file_size, "section header table offset past end of file");
    require(uint64_t(h.shnum) * entsize <= file_size - h.shoff,
            "section header table past end of file");

    for (uint16_t i = 0; i < h.shnum; ++i) {
        size_t p = size_t(h.shoff) + size_t(i) * entsize;
        raw_shdr s;
        if (is64) {
            s.name      = uint32_t(read_u(base, file_size, p + 0, 4));
            s.type      = uint32_t(read_u(base, file_size, p + 4, 4));
            s.flags     = read_u(base, file_size, p + 8, 8);
            s.addr      = read_u(base, file_size, p + 16, 8);
            s.offset    = read_u(base, file_size, p + 24, 8);
            s.size      = read_u(base, file_size, p + 32, 8);
            s.link      = uint32_t(read_u(base, file_size, p + 40, 4));
            s.info      = uint32_t(read_u(base, file_size, p + 44, 4));
            s.addralign = read_u(base, file_size, p + 48, 8);
            s.entsize   = read_u(base, file_size, p + 56, 8);
        } else {
            s.name      = uint32_t(read_u(base, file_size, p + 0, 4));
            s.type      = uint32_t(read_u(base, file_size, p + 4, 4));
            s.flags     = read_u(base, file_size, p + 8, 4);
            s.addr      = read_u(base, file_size, p + 12, 4);
            s.offset    = read_u(base, file_size, p + 16, 4);
            s.size      = read_u(base, file_size, p + 20, 4);
            s.link      = uint32_t(read_u(base, file_size, p + 24, 4));
            s.info      = uint32_t(read_u(base, file_size, p + 28, 4));
            s.addralign = read_u(base, file_size, p + 32, 4);
            s.entsize   = read_u(base, file_size, p + 36, 4);
        }
        out.push_back(s);
    }
    return out;
}

raw_phdr read_phdr(const uint8_t *base, size_t file_size, const raw_ehdr &h,
                   size_t index) {
    const bool is64 = (h.elf_class == ELFCLASS64);
    const size_t entsize = is64 ? sizeof(Elf64_Phdr) : sizeof(Elf32_Phdr);
    require(h.phoff <= file_size, "program header table offset past end of file");
    size_t p = size_t(h.phoff) + index * entsize;
    require(p + entsize <= file_size, "program header past end of file");

    raw_phdr ph;
    if (is64) {
        // ELF64 puts p_flags directly after p_type.
        ph.type    = uint32_t(read_u(base, file_size, p + 0, 4));
        ph.flags   = uint32_t(read_u(base, file_size, p + 4, 4));
        ph.offset  = read_u(base, file_size, p + 8, 8);
        ph.vaddr   = read_u(base, file_size, p + 16, 8);
        ph.paddr   = read_u(base, file_size, p + 24, 8);
        ph.filesz  = read_u(base, file_size, p + 32, 8);
        ph.memsz   = read_u(base, file_size, p + 40, 8);
        ph.align   = read_u(base, file_size, p + 48, 8);
    } else {
        // ELF32 keeps p_flags between p_memsz and p_align.
        ph.type    = uint32_t(read_u(base, file_size, p + 0, 4));
        ph.offset  = read_u(base, file_size, p + 4, 4);
        ph.vaddr   = read_u(base, file_size, p + 8, 4);
        ph.paddr   = read_u(base, file_size, p + 12, 4);
        ph.filesz  = read_u(base, file_size, p + 16, 4);
        ph.memsz   = read_u(base, file_size, p + 20, 4);
        ph.flags   = uint32_t(read_u(base, file_size, p + 24, 4));
        ph.align   = read_u(base, file_size, p + 28, 4);
    }
    return ph;
}

raw_sym read_sym(const uint8_t *base, size_t file_size, const raw_ehdr &h,
                 size_t record_off) {
    const bool is64 = (h.elf_class == ELFCLASS64);
    raw_sym s;
    if (is64) {
        // ELF64 carries st_info/st_other right after st_name.
        s.name  = uint32_t(read_u(base, file_size, record_off + 0, 4));
        s.info  = uint8_t(read_u(base, file_size, record_off + 4, 1));
        s.other = uint8_t(read_u(base, file_size, record_off + 5, 1));
        s.shndx = uint16_t(read_u(base, file_size, record_off + 6, 2));
        s.value = read_u(base, file_size, record_off + 8, 8);
        s.size  = read_u(base, file_size, record_off + 16, 8);
    } else {
        // ELF32 groups st_info/st_other/st_shndx after st_value/st_size.
        s.name  = uint32_t(read_u(base, file_size, record_off + 0, 4));
        s.value = read_u(base, file_size, record_off + 4, 4);
        s.size  = read_u(base, file_size, record_off + 8, 4);
        s.info  = uint8_t(read_u(base, file_size, record_off + 12, 1));
        s.other = uint8_t(read_u(base, file_size, record_off + 13, 1));
        s.shndx = uint16_t(read_u(base, file_size, record_off + 14, 2));
    }
    return s;
}

size_t native_sym_size(const raw_ehdr &h) {
    return h.elf_class == ELFCLASS64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
}

} // namespace

std::vector<section_t> Elf_parser::get_sections() {
    raw_ehdr ehdr = read_ehdr(m_mmap_program, m_file_size);
    std::vector<raw_shdr> shdrs = read_shdrs(m_mmap_program, m_file_size, ehdr);

    size_t shstr_off = 0;
    size_t shstr_size = 0;
    if (ehdr.shstrndx != SHN_UNDEF && ehdr.shstrndx < shdrs.size()) {
        const raw_shdr &strtab = shdrs[ehdr.shstrndx];
        require(strtab.offset + strtab.size <= m_file_size,
                "section name string table past end of file");
        shstr_off = size_t(strtab.offset);
        shstr_size = size_t(strtab.size);
    }

    std::vector<section_t> sections;
    for (uint32_t i = 0; i < shdrs.size(); ++i) {
        const raw_shdr &rh = shdrs[i];
        section_t section;
        section.section_index      = i;
        section.section_name       = (rh.name < shstr_size)
            ? std::string(str_at(m_mmap_program, m_file_size, shstr_off + rh.name))
            : std::string();
        section.section_type       = get_section_type(rh.type);
        section.section_type_id    = rh.type;
        section.section_addr       = rh.addr;
        section.section_offset     = rh.offset;
        section.section_size       = rh.size;
        section.section_link       = rh.link;
        section.section_info       = rh.info;
        section.section_flags      = rh.flags;
        section.section_ent_size   = rh.entsize;
        section.section_addr_align = rh.addralign;

        sections.push_back(section);
    }
    return sections;
}

std::vector<segment_t> Elf_parser::get_segments() {
    raw_ehdr ehdr = read_ehdr(m_mmap_program, m_file_size);

    std::vector<segment_t> segments;
    for (uint16_t i = 0; i < ehdr.phnum; ++i) {
        raw_phdr ph = read_phdr(m_mmap_program, m_file_size, ehdr, i);
        segment_t segment;
        segment.segment_type     = get_segment_type(ph.type);
        segment.segment_offset   = ph.offset;
        segment.segment_virtaddr = ph.vaddr;
        segment.segment_physaddr = ph.paddr;
        segment.segment_filesize = ph.filesz;
        segment.segment_memsize  = ph.memsz;
        segment.segment_flags    = get_segment_flags(ph.flags);
        segment.segment_align    = ph.align;

        segments.push_back(segment);
    }
    return segments;
}

std::vector<symbol_t> Elf_parser::get_symbols() {
    raw_ehdr ehdr = read_ehdr(m_mmap_program, m_file_size);
    std::vector<raw_shdr> shdrs = read_shdrs(m_mmap_program, m_file_size, ehdr);
    // Section names come from the same section header string table get_sections()
    // uses; keep one resolved copy instead of re-reading per record.
    std::vector<section_t> secs = get_sections();

    std::vector<symbol_t> symbols;
    // Symbol tables stay in physical section order; records (including the
    // mandatory null symbol at index zero) stay in their on-disk order.
    for (uint32_t si = 0; si < shdrs.size(); ++si) {
        const raw_shdr &tab = shdrs[si];
        if (tab.type != SHT_SYMTAB && tab.type != SHT_DYNSYM)
            continue;

        // Each symbol table names its own string table through sh_link.
        require(tab.link < shdrs.size(), "symbol table sh_link out of range");
        const raw_shdr &strtab = shdrs[tab.link];
        require(strtab.offset + strtab.size <= m_file_size,
                "symbol string table past end of file");
        size_t str_off = size_t(strtab.offset);
        size_t str_size = size_t(strtab.size);
        require(tab.offset + tab.size <= m_file_size,
                "symbol table past end of file");

        size_t ent = tab.entsize ? size_t(tab.entsize) : native_sym_size(ehdr);
        uint32_t total = uint32_t(tab.size / ent);

        for (uint32_t i = 0; i < total; ++i) {
            raw_sym rs = read_sym(m_mmap_program, m_file_size, ehdr,
                                  size_t(tab.offset) + size_t(i) * ent);
            symbol_t symbol;
            symbol.symbol_num        = i;
            symbol.symbol_table_index = si;
            symbol.symbol_value      = rs.value;
            symbol.symbol_size       = rs.size;
            symbol.symbol_type       = get_symbol_type(rs.info);
            symbol.symbol_bind       = get_symbol_bind(rs.info);
            symbol.symbol_visibility = get_symbol_visibility(rs.other);
            symbol.symbol_index      = get_symbol_index(rs.shndx);
            symbol.symbol_section    = secs[si].section_name;
            symbol.symbol_name = (rs.name < str_size)
                ? std::string(str_at(m_mmap_program, m_file_size,
                                     str_off + rs.name))
                : std::string();

            symbols.push_back(symbol);
        }
    }
    return symbols;
}

std::vector<relocation_t> Elf_parser::get_relocations() {
    raw_ehdr ehdr = read_ehdr(m_mmap_program, m_file_size);
    std::vector<raw_shdr> shdrs = read_shdrs(m_mmap_program, m_file_size, ehdr);
    std::vector<section_t> secs = get_sections();
    const bool is64 = (ehdr.elf_class == ELFCLASS64);

    // Legacy display field: the old .plt entry calculation, untouched.
    uint64_t plt_vma_address = 0;
    uint64_t plt_entry_size = 0;
    for (size_t i = 0; i < shdrs.size(); ++i) {
        if (secs[i].section_name == ".plt") {
            plt_vma_address = shdrs[i].addr;
            plt_entry_size = shdrs[i].entsize;
            break;
        }
    }

    std::vector<relocation_t> relocations;
    for (uint32_t ri = 0; ri < shdrs.size(); ++ri) {
        const raw_shdr &tab = shdrs[ri];
        if (tab.type != SHT_REL && tab.type != SHT_RELA)
            continue;
        const bool has_addend = (tab.type == SHT_RELA);
        require(tab.offset + tab.size <= m_file_size,
                "relocation table past end of file");

        // sh_link points at the symbol table owning every r_info symbol index.
        require(tab.link < shdrs.size(), "relocation sh_link out of range");
        const raw_shdr &symtab = shdrs[tab.link];
        require(symtab.type == SHT_SYMTAB || symtab.type == SHT_DYNSYM,
                "relocation sh_link does not reference a symbol table");
        require(symtab.link < shdrs.size(), "symbol table sh_link out of range");
        const raw_shdr &symstr = shdrs[symtab.link];
        require(symstr.offset + symstr.size <= m_file_size,
                "symbol string table past end of file");
        size_t str_off = size_t(symstr.offset);
        size_t str_size = size_t(symstr.size);

        size_t sym_ent = symtab.entsize ? size_t(symtab.entsize)
                                        : native_sym_size(ehdr);
        uint32_t sym_total = uint32_t(symtab.size / sym_ent);

        size_t ent = tab.entsize
            ? size_t(tab.entsize)
            : (has_addend ? (is64 ? sizeof(Elf64_Rela) : sizeof(Elf32_Rela))
                          : (is64 ? sizeof(Elf64_Rel)  : sizeof(Elf32_Rel)));
        uint32_t total = uint32_t(tab.size / ent);

        for (uint32_t i = 0; i < total; ++i) {
            size_t p = size_t(tab.offset) + size_t(i) * ent;
            uint64_t r_offset = read_u(m_mmap_program, m_file_size, p,
                                       is64 ? 8 : 4);
            uint64_t r_info = read_u(m_mmap_program, m_file_size,
                                     p + (is64 ? 8 : 4), is64 ? 8 : 4);

            // The split of r_info into symbol and type is class-specific.
            uint32_t r_sym, r_type;
            if (is64) {
                r_sym  = uint32_t(r_info >> 32);
                r_type = uint32_t(r_info & 0xffffffffu);
            } else {
                r_sym  = uint32_t(r_info >> 8);
                r_type = uint32_t(r_info & 0xffu);
            }

            int64_t addend = 0;
            if (has_addend) {
                uint64_t raw = read_u(m_mmap_program, m_file_size,
                                      p + (is64 ? 16 : 8), is64 ? 8 : 4);
                // RELA addends are signed; ELF32 stores a 32-bit Elf32_Sword.
                addend = is64 ? int64_t(raw) : sign_extend(raw, 32);
            }

            // Resolve the symbol through this table's own sh_link chain,
            // using the original record number rather than a global search.
            uint64_t sym_value = 0;
            std::string sym_name;
            if (r_sym < sym_total) {
                raw_sym rs = read_sym(m_mmap_program, m_file_size, ehdr,
                                      size_t(symtab.offset) +
                                      size_t(r_sym) * sym_ent);
                sym_value = rs.value;
                if (rs.name < str_size)
                    sym_name = std::string(str_at(m_mmap_program, m_file_size,
                                                  str_off + rs.name));
            } else {
                require(false, "relocation symbol index past symbol table");
            }

            relocation_t rel;
            rel.relocation_offset               = r_offset;
            rel.relocation_info                 = r_info;
            rel.relocation_type_id              = r_type;
            rel.relocation_type                 = get_relocation_type(r_type);
            rel.relocation_symbol_index         = r_sym;
            rel.relocation_symbol_table_index   = tab.link;
            rel.relocation_target_section_index = tab.info;
            rel.relocation_symbol_value         = sym_value;
            rel.relocation_symbol_name          = sym_name;
            rel.relocation_section_name         = secs[ri].section_name;
            rel.relocation_has_addend           = has_addend;
            rel.relocation_addend               = addend;
            rel.relocation_plt_address =
                plt_vma_address + uint64_t(i + 1) * plt_entry_size;

            relocations.push_back(rel);
        }
    }
    return relocations;
}

uint8_t *Elf_parser::get_memory_map() {
    return m_mmap_program;
}

void Elf_parser::load_memory_map() {
    int fd;
    struct stat st;

    if ((fd = open(m_program_path.c_str(), O_RDONLY)) < 0)
        throw std::runtime_error("elf_parser: cannot open file");
    if (fstat(fd, &st) < 0) {
        close(fd);
        throw std::runtime_error("elf_parser: cannot stat file");
    }
    m_file_size = size_t(st.st_size);
    m_mmap_program = static_cast<uint8_t*>(
        mmap(NULL, m_file_size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (m_mmap_program == MAP_FAILED) {
        m_mmap_program = nullptr;
        throw std::runtime_error("elf_parser: cannot mmap file");
    }

    // Validate identification up front and remember the file-declared format.
    raw_ehdr ehdr = read_ehdr(m_mmap_program, m_file_size);
    m_class = ehdr.elf_class;
    m_data = ehdr.data_encoding;
    m_machine = ehdr.machine;
}

std::string Elf_parser::get_section_type(uint32_t tt) {
    switch(tt) {
        case 0: return "SHT_NULL";      /* Section header table entry unused */
        case 1: return "SHT_PROGBITS";  /* Program data */
        case 2: return "SHT_SYMTAB";    /* Symbol table */
        case 3: return "SHT_STRTAB";    /* String table */
        case 4: return "SHT_RELA";      /* Relocation entries with addends */
        case 5: return "SHT_HASH";      /* Symbol hash table */
        case 6: return "SHT_DYNAMIC";   /* Dynamic linking information */
        case 7: return "SHT_NOTE";      /* Notes */
        case 8: return "SHT_NOBITS";    /* Program space with no data (bss) */
        case 9: return "SHT_REL";       /* Relocation entries, no addends */
        case 11: return "SHT_DYNSYM";   /* Dynamic linker symbol table */
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_segment_type(uint32_t seg_type) {
    switch(seg_type) {
        case PT_NULL:   return "NULL";                  /* Program header table entry unused */
        case PT_LOAD: return "LOAD";                    /* Loadable program segment */
        case PT_DYNAMIC: return "DYNAMIC";              /* Dynamic linking information */
        case PT_INTERP: return "INTERP";                /* Program interpreter */
        case PT_NOTE: return "NOTE";                    /* Auxiliary information */
        case PT_SHLIB: return "SHLIB";                  /* Reserved */
        case PT_PHDR: return "PHDR";                    /* Entry for header table itself */
        case PT_TLS: return "TLS";                      /* Thread-local storage segment */
        case PT_NUM: return "NUM";                      /* Number of defined types */
        case PT_LOOS: return "LOOS";                    /* Start of OS-specific */
        case PT_GNU_EH_FRAME: return "GNU_EH_FRAME";    /* GCC .eh_frame_hdr segment */
        case PT_GNU_STACK: return "GNU_STACK";          /* Indicates stack executability */
        case PT_GNU_RELRO: return "GNU_RELRO";          /* Read-only after relocation */
        case PT_SUNWBSS: return "SUNWBSS";              /* Sun Specific segment */
        case PT_SUNWSTACK: return "SUNWSTACK";          /* Stack segment */
        case PT_HIOS: return "HIOS";                    /* End of OS-specific */
        case PT_LOPROC: return "LOPROC";                /* Start of processor-specific */
        case PT_HIPROC: return "HIPROC";                /* End of processor-specific */
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_segment_flags(uint32_t seg_flags) {
    std::string flags;

    if(seg_flags & PF_R)
        flags.append("R");

    if(seg_flags & PF_W)
        flags.append("W");

    if(seg_flags & PF_X)
        flags.append("E");

    return flags;
}

std::string Elf_parser::get_symbol_type(uint8_t sym_type) {
    switch(ELF32_ST_TYPE(sym_type)) {
        case 0: return "NOTYPE";
        case 1: return "OBJECT";
        case 2: return "FUNC";
        case 3: return "SECTION";
        case 4: return "FILE";
        case 6: return "TLS";
        case 7: return "NUM";
        case 10: return "LOOS";
        case 12: return "HIOS";
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_symbol_bind(uint8_t sym_bind) {
    switch(ELF32_ST_BIND(sym_bind)) {
        case 0: return "LOCAL";
        case 1: return "GLOBAL";
        case 2: return "WEAK";
        case 3: return "NUM";
        case 10: return "UNIQUE";
        case 12: return "HIOS";
        case 13: return "LOPROC";
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_symbol_visibility(uint8_t sym_vis) {
    switch(ELF32_ST_VISIBILITY(sym_vis)) {
        case 0: return "DEFAULT";
        case 1: return "INTERNAL";
        case 2: return "HIDDEN";
        case 3: return "PROTECTED";
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_symbol_index(uint16_t sym_idx) {
    switch(sym_idx) {
        case SHN_ABS: return "ABS";
        case SHN_COMMON: return "COM";
        case SHN_UNDEF: return "UND";
        case SHN_XINDEX: return "COM";
        default: return std::to_string(sym_idx);
    }
}

std::string Elf_parser::get_relocation_type(uint32_t rela_type) {
    // Type ids share numbers across architectures; names follow e_machine.
    switch(m_machine) {
        case EM_386:
            switch(rela_type) {
                case 1: return "R_386_32";
                case 2: return "R_386_PC32";
                case 5: return "R_386_COPY";
                case 6: return "R_386_GLOB_DAT";
                case 7: return "R_386_JMP_SLOT";
                default: return "OTHERS";
            }
        case EM_X86_64:
            switch(rela_type) {
                case 1: return "R_X86_64_64";
                case 2: return "R_X86_64_PC32";
                case 5: return "R_X86_64_COPY";
                case 6: return "R_X86_64_GLOB_DAT";
                case 7: return "R_X86_64_JUMP_SLOT";
                default: return "OTHERS";
            }
        case EM_PPC:
            switch(rela_type) {
                case 1: return "R_PPC_ADDR32";
                case 26: return "R_PPC_REL32";
                default: return "OTHERS";
            }
        case EM_PPC64:
            switch(rela_type) {
                case 1: return "R_PPC64_ADDR32";
                case 26: return "R_PPC64_REL32";
                case 38: return "R_PPC64_ADDR64";
                case 44: return "R_PPC64_REL64";
                default: return "OTHERS";
            }
        default:
            return "OTHERS";
    }
}
