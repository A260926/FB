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

#include <stdexcept>

#include <fcntl.h>    /* O_RDONLY */
#include <unistd.h>   /* close */
#include <sys/stat.h> /* fstat */
#include <sys/mman.h> /* mmap, MAP_PRIVATE */

#include <elf.h>      // pinned format declarations (task_support/vendor/elf.h)

using namespace elf_parser;

Elf_parser::Elf_parser(std::string &program_path): m_program_path{program_path} {
    load_memory_map();
    try {
        parse_image();
    } catch (...) {
        munmap(m_mmap_program, static_cast<size_t>(m_file_size));
        m_mmap_program = nullptr;
        throw;
    }
}

Elf_parser::~Elf_parser() {
    if (m_mmap_program)
        munmap(m_mmap_program, static_cast<size_t>(m_file_size));
}

void Elf_parser::load_memory_map() {
    int fd = open(m_program_path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("elf_parser: cannot open " + m_program_path);

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        throw std::runtime_error("elf_parser: cannot stat " + m_program_path);
    }
    if (st.st_size <= 0) {
        close(fd);
        throw std::runtime_error("elf_parser: empty file " + m_program_path);
    }
    m_file_size = static_cast<uint64_t>(st.st_size);

    void *map = mmap(NULL, static_cast<size_t>(m_file_size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        m_mmap_program = nullptr;
        throw std::runtime_error("elf_parser: cannot map " + m_program_path);
    }
    m_mmap_program = static_cast<uint8_t*>(map);
}

void Elf_parser::parse_image() {
    if (m_file_size < EI_NIDENT ||
        m_mmap_program[0] != 0x7f || m_mmap_program[1] != 'E' ||
        m_mmap_program[2] != 'L' || m_mmap_program[3] != 'F')
        throw std::runtime_error("elf_parser: not an ELF file");

    m_elf_class = m_mmap_program[EI_CLASS];
    if (m_elf_class != ELFCLASS32 && m_elf_class != ELFCLASS64)
        throw std::runtime_error("elf_parser: unsupported ELF class");

    m_data_encoding = m_mmap_program[EI_DATA];
    if (m_data_encoding != ELFDATA2LSB && m_data_encoding != ELFDATA2MSB)
        throw std::runtime_error("elf_parser: unsupported ELF data encoding");

    m_is64 = (m_elf_class == ELFCLASS64);
    m_msb  = (m_data_encoding == ELFDATA2MSB);

    if (m_file_size < (m_is64 ? 64u : 52u))
        throw std::runtime_error("elf_parser: truncated ELF header");

    m_machine = fetch16(18);
    switch (m_machine) {
        case EM_386:
        case EM_X86_64:
        case EM_PPC:
        case EM_PPC64:
            break;
        default:
            throw std::runtime_error("elf_parser: unsupported ELF machine");
    }

    // The ELF header keeps the same field order in both classes; only the
    // address/offset words widen from 4 to 8 bytes at offset 24.
    if (m_is64) {
        m_phoff     = fetch64(32);
        m_shoff     = fetch64(40);
        m_phentsize = fetch16(54);
        m_phnum     = fetch16(56);
        m_shentsize = fetch16(58);
        m_shnum     = fetch16(60);
        m_shstrndx  = fetch16(62);
    } else {
        m_phoff     = fetch32(28);
        m_shoff     = fetch32(32);
        m_phentsize = fetch16(42);
        m_phnum     = fetch16(44);
        m_shentsize = fetch16(46);
        m_shnum     = fetch16(48);
        m_shstrndx  = fetch16(50);
    }

    const uint64_t shentsize = m_shentsize ? m_shentsize : (m_is64 ? 64 : 40);
    m_shdrs.reserve(m_shnum);
    for (uint32_t i = 0; i < m_shnum; ++i) {
        const uint64_t base = m_shoff + static_cast<uint64_t>(i) * shentsize;
        shdr_info sh;
        if (m_is64) {
            sh.name      = fetch32(base + 0);
            sh.type      = fetch32(base + 4);
            sh.flags     = fetch64(base + 8);
            sh.addr      = fetch64(base + 16);
            sh.offset    = fetch64(base + 24);
            sh.size      = fetch64(base + 32);
            sh.link      = fetch32(base + 40);
            sh.info      = fetch32(base + 44);
            sh.addralign = fetch64(base + 48);
            sh.entsize   = fetch64(base + 56);
        } else {
            sh.name      = fetch32(base + 0);
            sh.type      = fetch32(base + 4);
            sh.flags     = fetch32(base + 8);
            sh.addr      = fetch32(base + 12);
            sh.offset    = fetch32(base + 16);
            sh.size      = fetch32(base + 20);
            sh.link      = fetch32(base + 24);
            sh.info      = fetch32(base + 28);
            sh.addralign = fetch32(base + 32);
            sh.entsize   = fetch32(base + 36);
        }
        m_shdrs.push_back(sh);
    }
}

uint16_t Elf_parser::fetch16(uint64_t off) const {
    const uint8_t *p = m_mmap_program + off;
    if (m_msb)
        return static_cast<uint16_t>((p[0] << 8) | p[1]);
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t Elf_parser::fetch32(uint64_t off) const {
    const uint8_t *p = m_mmap_program + off;
    if (m_msb)
        return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
    return  static_cast<uint32_t>(p[0])        | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Elf_parser::fetch64(uint64_t off) const {
    const uint8_t *p = m_mmap_program + off;
    uint64_t v = 0;
    if (m_msb) {
        for (int i = 0; i < 8; ++i)
            v = (v << 8) | p[i];
    } else {
        for (int i = 7; i >= 0; --i)
            v = (v << 8) | p[i];
    }
    return v;
}

std::string Elf_parser::read_string(const shdr_info &strtab, uint32_t off) const {
    if (off >= strtab.size)
        return std::string();
    const char *const base = reinterpret_cast<const char*>(m_mmap_program + strtab.offset);
    const uint64_t room = strtab.size - off;
    uint64_t len = 0;
    while (len < room && base[off + len] != '\0')
        ++len;
    return std::string(base + off, static_cast<size_t>(len));
}

std::string Elf_parser::section_name(const shdr_info &sh) const {
    if (m_shstrndx >= m_shdrs.size())
        return std::string();
    return read_string(m_shdrs[m_shstrndx], sh.name);
}

std::vector<section_t> Elf_parser::get_sections() {
    std::vector<section_t> sections;
    for (uint32_t i = 0; i < m_shnum; ++i) {
        const shdr_info &sh = m_shdrs[i];
        section_t section;
        section.section_index      = i;
        section.section_name       = section_name(sh);
        section.section_type       = get_section_type(sh.type);
        section.section_type_id    = sh.type;
        section.section_addr       = sh.addr;
        section.section_offset     = sh.offset;
        section.section_size       = sh.size;
        section.section_ent_size   = sh.entsize;
        section.section_addr_align = sh.addralign;
        section.section_link       = sh.link;
        section.section_info       = sh.info;
        section.section_flags      = sh.flags;
        sections.push_back(section);
    }
    return sections;
}

std::vector<segment_t> Elf_parser::get_segments() {
    std::vector<segment_t> segments;
    const uint64_t phentsize = m_phentsize ? m_phentsize : (m_is64 ? 56 : 32);
    for (uint32_t i = 0; i < m_phnum; ++i) {
        const uint64_t base = m_phoff + static_cast<uint64_t>(i) * phentsize;
        uint32_t type, flags;
        segment_t segment;
        // p_flags sits at the end of the 32-bit entry but right after
        // p_type in the 64-bit entry.
        if (m_is64) {
            type  = fetch32(base + 0);
            flags = fetch32(base + 4);
            segment.segment_offset   = fetch64(base + 8);
            segment.segment_virtaddr = fetch64(base + 16);
            segment.segment_physaddr = fetch64(base + 24);
            segment.segment_filesize = fetch64(base + 32);
            segment.segment_memsize  = fetch64(base + 40);
            segment.segment_align    = fetch64(base + 48);
        } else {
            type  = fetch32(base + 0);
            flags = fetch32(base + 24);
            segment.segment_offset   = fetch32(base + 4);
            segment.segment_virtaddr = fetch32(base + 8);
            segment.segment_physaddr = fetch32(base + 12);
            segment.segment_filesize = fetch32(base + 16);
            segment.segment_memsize  = fetch32(base + 20);
            segment.segment_align    = fetch32(base + 28);
        }
        segment.segment_type  = get_segment_type(type);
        segment.segment_flags = get_segment_flags(flags);
        segments.push_back(segment);
    }
    return segments;
}

std::vector<Elf_parser::sym_info> Elf_parser::read_symbol_table(const shdr_info &tab) const {
    std::vector<sym_info> entries;
    const uint64_t entsize = tab.entsize ? tab.entsize : (m_is64 ? 24 : 16);
    if (entsize == 0)
        return entries;
    const uint64_t count = tab.size / entsize;
    entries.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        const uint64_t base = tab.offset + i * entsize;
        sym_info sym;
        // st_info/st_other sit between st_name and st_shndx in the 64-bit
        // entry but after st_value/st_size in the 32-bit entry.
        if (m_is64) {
            sym.name  = fetch32(base + 0);
            sym.info  = m_mmap_program[base + 4];
            sym.other = m_mmap_program[base + 5];
            sym.shndx = fetch16(base + 6);
            sym.value = fetch64(base + 8);
            sym.size  = fetch64(base + 16);
        } else {
            sym.name  = fetch32(base + 0);
            sym.value = fetch32(base + 4);
            sym.size  = fetch32(base + 8);
            sym.info  = m_mmap_program[base + 12];
            sym.other = m_mmap_program[base + 13];
            sym.shndx = fetch16(base + 14);
        }
        entries.push_back(sym);
    }
    return entries;
}

std::vector<symbol_t> Elf_parser::get_symbols() {
    std::vector<symbol_t> symbols;
    for (uint32_t i = 0; i < m_shnum; ++i) {
        const shdr_info &tab = m_shdrs[i];
        if (tab.type != SHT_SYMTAB && tab.type != SHT_DYNSYM)
            continue;

        // The strings of a symbol table live in the section its own sh_link
        // points to; the table name is irrelevant.
        const shdr_info &strtab = (tab.link < m_shnum) ? m_shdrs[tab.link] : m_shdrs[0];
        const std::vector<sym_info> entries = read_symbol_table(tab);
        const std::string tab_name = section_name(tab);

        for (uint32_t j = 0; j < entries.size(); ++j) {
            symbol_t symbol;
            symbol.symbol_num         = j;
            symbol.symbol_table_index = i;
            symbol.symbol_value       = entries[j].value;
            symbol.symbol_size        = entries[j].size;
            symbol.symbol_type        = get_symbol_type(entries[j].info);
            symbol.symbol_bind        = get_symbol_bind(entries[j].info);
            symbol.symbol_visibility  = get_symbol_visibility(entries[j].other);
            symbol.symbol_index       = get_symbol_index(entries[j].shndx);
            symbol.symbol_section     = tab_name;
            symbol.symbol_name        = read_string(strtab, entries[j].name);
            symbols.push_back(symbol);
        }
    }
    return symbols;
}

std::vector<relocation_t> Elf_parser::get_relocations() {
    // Legacy display field: address of the would-be PLT entry for this
    // relocation, computed from the .plt section as before.
    uint64_t plt_entry_size = 0, plt_vma_address = 0;
    for (uint32_t i = 0; i < m_shnum; ++i) {
        if (section_name(m_shdrs[i]) == ".plt") {
            plt_entry_size  = m_shdrs[i].entsize;
            plt_vma_address = m_shdrs[i].addr;
            break;
        }
    }

    std::vector<relocation_t> relocations;
    for (uint32_t i = 0; i < m_shnum; ++i) {
        const shdr_info &sec = m_shdrs[i];
        const bool is_rela = (sec.type == SHT_RELA);
        const bool is_rel  = (sec.type == SHT_REL);
        if (!is_rela && !is_rel)
            continue;

        // The relocation section's own sh_link selects its symbol table, and
        // that table's sh_link selects the strings for the symbol names.
        const shdr_info &symtab = (sec.link < m_shnum)     ? m_shdrs[sec.link]     : m_shdrs[0];
        const shdr_info &strtab = (symtab.link < m_shnum)  ? m_shdrs[symtab.link]  : m_shdrs[0];
        const std::vector<sym_info> syms = read_symbol_table(symtab);

        const uint64_t entsize = sec.entsize ? sec.entsize
            : (m_is64 ? (is_rela ? 24 : 16) : (is_rela ? 12 : 8));
        if (entsize == 0)
            continue;
        const uint64_t count = sec.size / entsize;
        const std::string sec_name = section_name(sec);

        for (uint64_t j = 0; j < count; ++j) {
            const uint64_t base = sec.offset + j * entsize;
            relocation_t rel;
            uint64_t r_info;
            if (m_is64) {
                rel.relocation_offset = fetch64(base + 0);
                r_info                = fetch64(base + 8);
                if (is_rela)
                    rel.relocation_addend = static_cast<int64_t>(fetch64(base + 16));
            } else {
                rel.relocation_offset = fetch32(base + 0);
                r_info                = fetch32(base + 4);
                if (is_rela)
                    rel.relocation_addend = static_cast<int32_t>(fetch32(base + 8));
            }
            rel.relocation_info = r_info;

            // r_info packs symbol and type per class: the symbol sits in the
            // high 32 bits for ELF64, above the low type byte for ELF32.
            if (m_is64) {
                rel.relocation_symbol_index = static_cast<uint32_t>(r_info >> 32);
                rel.relocation_type_id      = static_cast<uint32_t>(r_info & 0xffffffffu);
            } else {
                rel.relocation_symbol_index = static_cast<uint32_t>(r_info >> 8);
                rel.relocation_type_id      = static_cast<uint32_t>(r_info & 0xffu);
            }
            rel.relocation_type = get_relocation_type(rel.relocation_type_id);

            rel.relocation_symbol_table_index   = sec.link;
            rel.relocation_target_section_index = sec.info;
            if (rel.relocation_symbol_index < syms.size()) {
                const sym_info &sym = syms[rel.relocation_symbol_index];
                rel.relocation_symbol_value = sym.value;
                rel.relocation_symbol_name  = read_string(strtab, sym.name);
            }

            // RELA carries an explicit signed addend; REL has none and the
            // target bytes are never interpreted as an implicit one.
            rel.relocation_has_addend = is_rela;
            if (!is_rela)
                rel.relocation_addend = 0;

            rel.relocation_plt_address  = plt_vma_address + (j + 1) * plt_entry_size;
            rel.relocation_section_name = sec_name;
            relocations.push_back(rel);
        }
    }
    return relocations;
}

uint8_t *Elf_parser::get_memory_map() {
    return m_mmap_program;
}

std::string Elf_parser::get_section_type(uint32_t tt) const {
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

std::string Elf_parser::get_segment_type(uint32_t seg_type) const {
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
        //case PT_LOSUNW: return "LOSUNW";
        case PT_SUNWBSS: return "SUNWBSS";              /* Sun Specific segment */
        case PT_SUNWSTACK: return "SUNWSTACK";          /* Stack segment */
        //case PT_HISUNW: return "HISUNW";
        case PT_HIOS: return "HIOS";                    /* End of OS-specific */
        case PT_LOPROC: return "LOPROC";                /* Start of processor-specific */
        case PT_HIPROC: return "HIPROC";                /* End of processor-specific */
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_segment_flags(uint32_t seg_flags) const {
    std::string flags;

    if(seg_flags & PF_R)
        flags.append("R");

    if(seg_flags & PF_W)
        flags.append("W");

    if(seg_flags & PF_X)
        flags.append("E");

    return flags;
}

std::string Elf_parser::get_symbol_type(uint8_t sym_info) const {
    switch(ELF32_ST_TYPE(sym_info)) {
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

std::string Elf_parser::get_symbol_bind(uint8_t sym_info) const {
    switch(ELF32_ST_BIND(sym_info)) {
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

std::string Elf_parser::get_symbol_visibility(uint8_t sym_other) const {
    switch(ELF32_ST_VISIBILITY(sym_other)) {
        case 0: return "DEFAULT";
        case 1: return "INTERNAL";
        case 2: return "HIDDEN";
        case 3: return "PROTECTED";
        default: return "UNKNOWN";
    }
}

std::string Elf_parser::get_symbol_index(uint16_t sym_idx) const {
    switch(sym_idx) {
        case SHN_ABS: return "ABS";
        case SHN_COMMON: return "COM";
        case SHN_UNDEF: return "UND";
        case SHN_XINDEX: return "COM";
        default: return std::to_string(sym_idx);
    }
}

std::string Elf_parser::get_relocation_type(uint32_t type_id) const {
    switch (m_machine) {
        case EM_386:
            switch (type_id) {
                case R_386_32:       return "R_386_32";
                case R_386_PC32:     return "R_386_PC32";
                case R_386_COPY:     return "R_386_COPY";
                case R_386_GLOB_DAT: return "R_386_GLOB_DAT";
                case R_386_JMP_SLOT: return "R_386_JUMP_SLOT";
            }
            break;
        case EM_X86_64:
            switch (type_id) {
                case R_X86_64_64:        return "R_X86_64_64";
                case R_X86_64_PC32:      return "R_X86_64_PC32";
                case R_X86_64_COPY:      return "R_X86_64_COPY";
                case R_X86_64_GLOB_DAT:  return "R_X86_64_GLOB_DAT";
                case R_X86_64_JUMP_SLOT: return "R_X86_64_JUMP_SLOT";
            }
            break;
        case EM_PPC:
            switch (type_id) {
                case R_PPC_ADDR32: return "R_PPC_ADDR32";
                case R_PPC_REL32:  return "R_PPC_REL32";
            }
            break;
        case EM_PPC64:
            switch (type_id) {
                case R_PPC64_ADDR32: return "R_PPC64_ADDR32";
                case R_PPC64_REL32:  return "R_PPC64_REL32";
                case R_PPC64_ADDR64: return "R_PPC64_ADDR64";
                case R_PPC64_REL64:  return "R_PPC64_REL64";
            }
            break;
    }
    return "OTHERS";
}
