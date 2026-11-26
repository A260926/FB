// Regression tests for the class/endian-aware ELF parser.
//
// Every fixture is synthesized byte by byte: the same builder emits ELF32/ELF64
// in either endianness, so a parser that only byte-swaps but keeps the host
// ELF64 field order fails these cases (program header flags, symbol info/other
// positions, r_info splitting, signed addends).
#include "elf_parser.hpp"
#include "task_support/check.h"
#include "task_support/fixture.hh"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>

using elf_parser::Elf_parser;
using sample::bytes;

namespace {

// Target description for one synthesized object.
struct BuildCfg {
    bool is64;
    bool big;
    uint16_t machine;
    uint64_t base;       // .text / segment base address
    uint64_t helper_val; // value of the second .symtab symbol
    uint32_t t_addr;     // relocation type id: address relocation
    uint32_t t_pc;       // relocation type id: pc-relative relocation
    uint32_t t_dyn;      // relocation type id: dynamic relocation
};

// Wrapper around the fixture's little-endian put(): reverse the bytes when the
// target object is big-endian.
void put(bytes &b, size_t p, uint64_t v, unsigned n, bool big) {
    sample::put(b, p, v, n);
    if (big)
        std::reverse(b.begin() + p, b.begin() + p + n);
}

struct SecRec {
    std::string name;
    uint32_t type;
    uint64_t flags, addr, off, size, link, info, align_, entsize;
};

// Builds a complete relocatable object with two program headers, two string
// tables, a .symtab (null + anchor + helper), a .dynsym (null + dynsym_name),
// one RELA section per symbol table and one REL section. Nothing is executed
// and no relocation is applied; the bytes are static test input only.
bytes build_elf(const BuildCfg &c) {
    const size_t H      = c.is64 ? 64 : 52;
    const size_t phent  = c.is64 ? 56 : 32;
    const size_t shent  = c.is64 ? 64 : 40;
    const size_t symt   = c.is64 ? 24 : 16;
    const size_t relat  = c.is64 ? 24 : 12;
    const size_t relt   = c.is64 ? 16 : 8;
    const unsigned align = c.is64 ? 8 : 4;

    bytes b(H, 0);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = c.is64 ? ELFCLASS64 : ELFCLASS32;
    b[5] = c.big ? ELFDATA2MSB : ELFDATA2LSB;
    b[6] = 1;

    auto P = [&](size_t p, uint64_t v, unsigned n) { put(b, p, v, n, c.big); };
    P(16, 1, 2);                 // e_type = ET_REL
    P(18, c.machine, 2);
    P(20, 1, 4);                 // e_version
    if (c.is64) {
        P(24, c.base, 8);        // e_entry
        P(32, H, 8);             // e_phoff directly follows the header
        P(48, 0, 4);             // e_flags
        P(52, H, 2);             // e_ehsize
        P(54, phent, 2); P(56, 2, 2);
        P(58, shent, 2); P(60, 10, 2); P(62, 1, 2);
    } else {
        P(24, c.base, 4);
        P(28, H, 4);
        P(36, 0, 4);
        P(40, H, 2);
        P(42, phent, 2); P(44, 2, 2);
        P(46, shent, 2); P(48, 10, 2); P(50, 1, 2);
    }

    // Program headers: a loadable R+E segment and a R+W GNU_STACK segment.
    // The p_flags slot itself differs between the classes, which is the point.
    b.resize(H + 2 * phent, 0);
    auto write_phdr = [&](size_t idx, uint32_t type, uint32_t flags,
                          uint64_t off, uint64_t vaddr, uint64_t paddr,
                          uint64_t filesz, uint64_t memsz, uint64_t al) {
        size_t p = H + idx * phent;
        if (c.is64) {
            P(p + 0, type, 4);  P(p + 4, flags, 4);
            P(p + 8, off, 8);   P(p + 16, vaddr, 8); P(p + 24, paddr, 8);
            P(p + 32, filesz, 8); P(p + 40, memsz, 8); P(p + 48, al, 8);
        } else {
            P(p + 0, type, 4);  P(p + 4, off, 4);
            P(p + 8, vaddr, 4); P(p + 12, paddr, 4);
            P(p + 16, filesz, 4); P(p + 20, memsz, 4);
            P(p + 24, flags, 4); P(p + 28, al, 4);
        }
    };
    write_phdr(0, PT_LOAD, PF_R | PF_X, 0, c.base, c.base, 0x200, 0x200, 0x1000);
    write_phdr(1, PT_GNU_STACK, PF_R | PF_W, 0, 0, 0, 0, 0, 0x10);

    size_t cursor = H + 2 * phent;
    auto align_up = [&](size_t v, unsigned a) { return (v + a - 1) & ~(size_t(a) - 1); };
    auto add_blob = [&](const bytes &blob, unsigned a) -> size_t {
        size_t off = align_up(cursor, a);
        if (off + blob.size() > b.size())
            b.resize(off + blob.size(), 0);
        std::memcpy(b.data() + off, blob.data(), blob.size());
        cursor = off + blob.size();
        return off;
    };

    // .text payload (never executed).
    bytes text;
    text.push_back(0x11); text.push_back(0x22);
    text.push_back(0x33); text.push_back(0x44);

    bytes strtab;
    std::map<std::string, uint32_t> stroff;
    auto add_string = [&](bytes &tab, std::map<std::string, uint32_t> &offs,
                          const std::string &s) -> uint32_t {
        std::map<std::string, uint32_t>::iterator it = offs.find(s);
        if (it != offs.end())
            return it->second;
        uint32_t at = uint32_t(tab.size());
        offs[s] = at;
        tab.insert(tab.end(), s.begin(), s.end());
        tab.push_back(0);
        return at;
    };
    add_string(strtab, stroff, "");
    uint32_t anchor_name = add_string(strtab, stroff, "anchor");
    uint32_t helper_name = add_string(strtab, stroff, "helper");

    bytes dynstr;
    std::map<std::string, uint32_t> dynstroff;
    add_string(dynstr, dynstroff, "");
    uint32_t dynn_name = add_string(dynstr, dynstroff, "dynsym_name");

    auto write_sym = [&](bytes &tab, uint32_t i, uint32_t name, uint8_t info,
                         uint8_t other, uint16_t shndx, uint64_t value,
                         uint64_t size) {
        tab.resize((i + 1) * symt, 0);
        size_t p = size_t(i) * symt;
        if (c.is64) {
            put(tab, p + 0, name, 4, c.big);
            put(tab, p + 4, info, 1, c.big);
            put(tab, p + 5, other, 1, c.big);
            put(tab, p + 6, shndx, 2, c.big);
            put(tab, p + 8, value, 8, c.big);
            put(tab, p + 16, size, 8, c.big);
        } else {
            put(tab, p + 0, name, 4, c.big);
            put(tab, p + 4, value, 4, c.big);
            put(tab, p + 8, size, 4, c.big);
            put(tab, p + 12, info, 1, c.big);
            put(tab, p + 13, other, 1, c.big);
            put(tab, p + 14, shndx, 2, c.big);
        }
    };
    bytes symtab;
    write_sym(symtab, 0, 0, 0, 0, SHN_UNDEF, 0, 0);
    write_sym(symtab, 1, anchor_name, ELF32_ST_INFO(STB_GLOBAL, STT_FUNC),
              STV_HIDDEN, 2, c.base + 2, 0x11);
    write_sym(symtab, 2, helper_name, ELF32_ST_INFO(STB_LOCAL, STT_OBJECT),
              STV_DEFAULT, SHN_ABS, c.helper_val, 0x22);

    bytes dynsym;
    write_sym(dynsym, 0, 0, 0, 0, SHN_UNDEF, 0, 0);
    write_sym(dynsym, 1, dynn_name, ELF32_ST_INFO(STB_GLOBAL, STT_FUNC),
              STV_DEFAULT, SHN_UNDEF, c.base + 0x100, 8);

    auto rinfo = [&](uint32_t sym, uint32_t type) -> uint64_t {
        return c.is64 ? ((uint64_t(sym) << 32) | type)
                      : uint64_t((sym << 8) | (type & 0xff));
    };
    auto write_rela = [&](bytes &tab, uint32_t i, uint64_t off, uint32_t sym,
                          uint32_t type, int64_t addend) {
        tab.resize((i + 1) * relat, 0);
        size_t p = size_t(i) * relat;
        if (c.is64) {
            put(tab, p + 0, off, 8, c.big);
            put(tab, p + 8, rinfo(sym, type), 8, c.big);
            put(tab, p + 16, uint64_t(addend), 8, c.big);
        } else {
            put(tab, p + 0, off, 4, c.big);
            put(tab, p + 4, rinfo(sym, type), 4, c.big);
            put(tab, p + 8, uint64_t(uint32_t(addend)), 4, c.big);
        }
    };
    auto write_rel = [&](bytes &tab, uint64_t off, uint32_t sym, uint32_t type) {
        tab.resize(relt, 0);
        if (c.is64) {
            put(tab, 0, off, 8, c.big);
            put(tab, 8, rinfo(sym, type), 8, c.big);
        } else {
            put(tab, 0, off, 4, c.big);
            put(tab, 4, rinfo(sym, type), 4, c.big);
        }
    };
    bytes rela_text;
    write_rela(rela_text, 0, c.base, 1, c.t_addr, -4);
    write_rela(rela_text, 1, c.base + 1, 2, c.t_pc, 0x55);
    bytes rel_text;
    write_rel(rel_text, c.base + 3, 1, c.t_pc);
    bytes rela_dyn;
    write_rela(rela_dyn, 0, c.base + 0x80, 1, c.t_dyn, -8);

    // Section header string table, built from the final section list.
    static const char *const names[] = {
        "", ".shstrtab", ".text", ".strtab", ".symtab", ".dynsym",
        ".dynstr", ".rela.text", ".rel.text", ".rela.dyn"
    };
    bytes shstr;
    std::map<std::string, uint32_t> shstroff;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        add_string(shstr, shstroff, names[i]);

    // Lay out content blobs and remember their file offsets.
    size_t text_off     = add_blob(text, 1);
    size_t strtab_off   = add_blob(strtab, 1);
    size_t dynstr_off   = add_blob(dynstr, 1);
    size_t symtab_off   = add_blob(symtab, align);
    size_t dynsym_off   = add_blob(dynsym, align);
    size_t relatext_off = add_blob(rela_text, align);
    size_t reltext_off  = add_blob(rel_text, align);
    size_t reladyn_off  = add_blob(rela_dyn, align);
    size_t shstr_off    = add_blob(shstr, 1);

    SecRec recs[10];
    recs[0] = {"",          0,          0, 0, 0, 0, 0, 0, 0, 0};
    recs[1] = {".shstrtab", SHT_STRTAB, 0, 0, shstr_off, shstr.size(), 0, 0, 1, 0};
    recs[2] = {".text",     SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, c.base,
               text_off, text.size(), 0, 0, 1, 0};
    recs[3] = {".strtab",   SHT_STRTAB, 0, 0, strtab_off, strtab.size(), 0, 0, 1, 0};
    recs[4] = {".symtab",   SHT_SYMTAB, 0, 0, symtab_off, symtab.size(), 3, 1, align, symt};
    recs[5] = {".dynsym",   SHT_DYNSYM, 0, 0, dynsym_off, dynsym.size(), 6, 1, align, symt};
    recs[6] = {".dynstr",   SHT_STRTAB, 0, 0, dynstr_off, dynstr.size(), 0, 0, 1, 0};
    recs[7] = {".rela.text", SHT_RELA, 0, 0, relatext_off, rela_text.size(), 4, 2, align, relat};
    recs[8] = {".rel.text", SHT_REL, 0, 0, reltext_off, rel_text.size(), 4, 2, align, relt};
    recs[9] = {".rela.dyn", SHT_RELA, 0, 0, reladyn_off, rela_dyn.size(), 5, 2, align, relat};

    size_t shoff = align_up(cursor, align);
    b.resize(shoff + 10 * shent, 0);
    if (c.is64)
        P(40, shoff, 8);
    else
        P(32, shoff, 4);

    for (uint32_t i = 0; i < 10; ++i) {
        const SecRec &r = recs[i];
        size_t p = shoff + size_t(i) * shent;
        P(p + 0, shstroff[r.name], 4);
        if (c.is64) {
            P(p + 4, r.type, 4);  P(p + 8, r.flags, 8);
            P(p + 16, r.addr, 8); P(p + 24, r.off, 8); P(p + 32, r.size, 8);
            P(p + 40, r.link, 4); P(p + 44, r.info, 4);
            P(p + 48, r.align_, 8); P(p + 56, r.entsize, 8);
        } else {
            P(p + 4, r.type, 4);  P(p + 8, r.flags, 4);
            P(p + 12, r.addr, 4); P(p + 16, r.off, 4); P(p + 20, r.size, 4);
            P(p + 24, r.link, 4); P(p + 28, r.info, 4);
            P(p + 32, r.align_, 4); P(p + 36, r.entsize, 4);
        }
    }
    return b;
}

BuildCfg cfg_i386() {
    return BuildCfg{false, false, EM_386,   0x00011000ULL, 0xDEADBEEFULL, 1, 2, 6};
}
BuildCfg cfg_ppc32() {
    return BuildCfg{false, true,  EM_PPC,   0x00011000ULL, 0xDEADBEEFULL, 1, 26, 26};
}
BuildCfg cfg_x64() {
    return BuildCfg{true,  false, EM_X86_64, 0x100000401000ULL, 0x123456789ABCULL, 1, 2, 6};
}
BuildCfg cfg_ppc64() {
    return BuildCfg{true,  true,  EM_PPC64, 0x100000401000ULL, 0x123456789ABCULL, 38, 44, 1};
}

std::string save_fixture(const std::string &name, const bytes &b) {
    std::string path = std::string(".task-build/") + name;
    sample::save(path, b);
    return path;
}

bytes load_file(const std::string &path) {
    std::ifstream in(path.c_str(), std::ios::binary);
    return bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const char *addr_type_name(uint16_t machine) {
    switch (machine) {
        case EM_386:    return "R_386_32";
        case EM_PPC:    return "R_PPC_ADDR32";
        case EM_X86_64: return "R_X86_64_64";
        case EM_PPC64:  return "R_PPC64_ADDR64";
        default:        return "OTHERS";
    }
}
const char *pc_type_name(uint16_t machine) {
    switch (machine) {
        case EM_386:    return "R_386_PC32";
        case EM_PPC:    return "R_PPC_REL32";
        case EM_X86_64: return "R_X86_64_PC32";
        case EM_PPC64:  return "R_PPC64_REL64";
        default:        return "OTHERS";
    }
}
const char *dyn_type_name(uint16_t machine) {
    switch (machine) {
        case EM_386:    return "R_386_GLOB_DAT";
        case EM_PPC:    return "R_PPC_REL32";
        case EM_X86_64: return "R_X86_64_GLOB_DAT";
        case EM_PPC64:  return "R_PPC64_ADDR32";
        default:        return "OTHERS";
    }
}

// Checks shared by every synthesized target: raw layout, widened values and
// per-class field positions. Taken by value so the non-const reference
// constructor of the legacy API has an lvalue to bind to.
int check_layout(const BuildCfg &c, std::string path) {
    Elf_parser parser(path);
    TASK_CHECK(parser.elf_class() == (c.is64 ? ELFCLASS64 : ELFCLASS32));
    TASK_CHECK(parser.data_encoding() == (c.big ? ELFDATA2MSB : ELFDATA2LSB));
    TASK_CHECK(parser.machine() == c.machine);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 10);
    static const char *const expect_names[10] = {
        "", ".shstrtab", ".text", ".strtab", ".symtab", ".dynsym",
        ".dynstr", ".rela.text", ".rel.text", ".rela.dyn"
    };
    for (size_t i = 0; i < 10; ++i) {
        TASK_CHECK(secs[i].section_name == expect_names[i]);
        TASK_CHECK(secs[i].section_index == i);
    }
    TASK_CHECK(secs[2].section_type == "SHT_PROGBITS");
    TASK_CHECK(secs[2].section_type_id == SHT_PROGBITS);
    TASK_CHECK(secs[2].section_flags == (SHF_ALLOC | SHF_EXECINSTR));
    TASK_CHECK(secs[2].section_addr == c.base);
    TASK_CHECK(secs[2].section_size == 4);
    TASK_CHECK(secs[4].section_type == "SHT_SYMTAB");
    TASK_CHECK(secs[4].section_link == 3);
    TASK_CHECK(secs[4].section_info == 1);
    TASK_CHECK(secs[4].section_ent_size == (c.is64 ? 24 : 16));
    TASK_CHECK(secs[4].section_size == uint64_t(3) * (c.is64 ? 24 : 16));
    TASK_CHECK(secs[5].section_type == "SHT_DYNSYM");
    TASK_CHECK(secs[5].section_link == 6);
    TASK_CHECK(secs[7].section_type == "SHT_RELA");
    TASK_CHECK(secs[7].section_link == 4);
    TASK_CHECK(secs[7].section_info == 2);
    TASK_CHECK(secs[7].section_ent_size == (c.is64 ? 24 : 12));
    TASK_CHECK(secs[7].section_size == uint64_t(2) * (c.is64 ? 24 : 12));
    TASK_CHECK(secs[8].section_type == "SHT_REL");
    TASK_CHECK(secs[8].section_ent_size == (c.is64 ? 16 : 8));
    TASK_CHECK(secs[8].section_size == (c.is64 ? 16 : 8));
    TASK_CHECK(secs[9].section_link == 5);

    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    TASK_CHECK(segs.size() == 2);
    TASK_CHECK(segs[0].segment_type == "LOAD");
    // Flags only decode correctly when the class-specific p_flags slot is used.
    TASK_CHECK(segs[0].segment_flags == "RE");
    TASK_CHECK(segs[0].segment_offset == 0);
    TASK_CHECK(segs[0].segment_virtaddr == c.base);
    TASK_CHECK(segs[0].segment_physaddr == c.base);
    TASK_CHECK(segs[0].segment_filesize == 0x200);
    TASK_CHECK(segs[0].segment_memsize == 0x200);
    TASK_CHECK(segs[0].segment_align == 0x1000);
    TASK_CHECK(segs[1].segment_type == "GNU_STACK");
    TASK_CHECK(segs[1].segment_flags == "RW");
    TASK_CHECK(segs[1].segment_virtaddr == 0);
    TASK_CHECK(segs[1].segment_align == 0x10);

    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 5);
    // Null symbol, physical table order and original record order preserved.
    TASK_CHECK(syms[0].symbol_num == 0 && syms[0].symbol_table_index == 4);
    TASK_CHECK(syms[0].symbol_name.empty() && syms[0].symbol_value == 0);
    TASK_CHECK(syms[0].symbol_type == "NOTYPE" && syms[0].symbol_bind == "LOCAL");
    TASK_CHECK(syms[0].symbol_visibility == "DEFAULT" && syms[0].symbol_index == "UND");
    TASK_CHECK(syms[1].symbol_num == 1 && syms[1].symbol_table_index == 4);
    TASK_CHECK(syms[1].symbol_name == "anchor");
    TASK_CHECK(syms[1].symbol_value == c.base + 2);
    TASK_CHECK(syms[1].symbol_size == 0x11);
    // st_info/st_other positions differ between the classes.
    TASK_CHECK(syms[1].symbol_type == "FUNC");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(syms[1].symbol_visibility == "HIDDEN");
    TASK_CHECK(syms[1].symbol_index == "2");
    TASK_CHECK(syms[1].symbol_section == ".symtab");
    TASK_CHECK(syms[2].symbol_name == "helper");
    TASK_CHECK(syms[2].symbol_value == c.helper_val);
    TASK_CHECK(syms[2].symbol_size == 0x22);
    TASK_CHECK(syms[2].symbol_type == "OBJECT");
    TASK_CHECK(syms[2].symbol_bind == "LOCAL");
    TASK_CHECK(syms[2].symbol_index == "ABS");
    TASK_CHECK(syms[3].symbol_num == 0 && syms[3].symbol_table_index == 5);
    TASK_CHECK(syms[3].symbol_name.empty());
    TASK_CHECK(syms[3].symbol_section == ".dynsym");
    TASK_CHECK(syms[4].symbol_num == 1 && syms[4].symbol_table_index == 5);
    TASK_CHECK(syms[4].symbol_name == "dynsym_name");
    TASK_CHECK(syms[4].symbol_value == c.base + 0x100);
    TASK_CHECK(syms[4].symbol_size == 8);
    TASK_CHECK(syms[4].symbol_index == "UND");

    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 4);
    TASK_CHECK(rels[0].relocation_section_name == ".rela.text");
    TASK_CHECK(rels[0].relocation_offset == c.base);
    TASK_CHECK(rels[0].relocation_symbol_index == 1);
    TASK_CHECK(rels[0].relocation_type_id == c.t_addr);
    TASK_CHECK(rels[0].relocation_type == addr_type_name(c.machine));
    TASK_CHECK(rels[0].relocation_symbol_table_index == 4);
    TASK_CHECK(rels[0].relocation_target_section_index == 2);
    TASK_CHECK(rels[0].relocation_has_addend == true);
    TASK_CHECK(rels[0].relocation_addend == -4); // sign-extended, incl. 32-bit
    TASK_CHECK(rels[1].relocation_offset == c.base + 1);
    TASK_CHECK(rels[1].relocation_symbol_index == 2);
    TASK_CHECK(rels[1].relocation_type_id == c.t_pc);
    TASK_CHECK(rels[1].relocation_type == pc_type_name(c.machine));
    TASK_CHECK(rels[1].relocation_addend == 0x55);
    TASK_CHECK(rels[2].relocation_section_name == ".rel.text");
    TASK_CHECK(rels[2].relocation_has_addend == false);
    TASK_CHECK(rels[2].relocation_addend == 0); // REL: no explicit addend
    TASK_CHECK(rels[2].relocation_offset == c.base + 3);
    TASK_CHECK(rels[2].relocation_type_id == c.t_pc);
    TASK_CHECK(rels[3].relocation_section_name == ".rela.dyn");
    TASK_CHECK(rels[3].relocation_has_addend == true);
    TASK_CHECK(rels[3].relocation_addend == -8);
    TASK_CHECK(rels[3].relocation_offset == c.base + 0x80);
    TASK_CHECK(rels[3].relocation_type_id == c.t_dyn);
    TASK_CHECK(rels[3].relocation_type == dyn_type_name(c.machine));
    return 0;
}

int run_i386_layout() {
    BuildCfg c = cfg_i386();
    return check_layout(c, save_fixture("i386.elf", build_elf(c)));
}
int run_ppc32_layout() {
    BuildCfg c = cfg_ppc32();
    return check_layout(c, save_fixture("ppc32.elf", build_elf(c)));
}
int run_x64_layout() {
    BuildCfg c = cfg_x64();
    return check_layout(c, save_fixture("x64.elf", build_elf(c)));
}
int run_ppc64_layout() {
    BuildCfg c = cfg_ppc64();
    return check_layout(c, save_fixture("ppc64.elf", build_elf(c)));
}

// ---- references: sh_link linkage, r_info splitting, interleave safety ----

int run_relocation_references() {
    BuildCfg c = cfg_ppc32(); // big-endian ELF32: strictest r_info split case
    std::string path = save_fixture("ref_ppc32.elf", build_elf(c));
    Elf_parser parser(path);
    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 4);

    // .rela.text follows its own sh_link to .symtab entry 1/2.
    TASK_CHECK(rels[0].relocation_symbol_name == "anchor");
    TASK_CHECK(rels[0].relocation_symbol_value == c.base + 2);
    TASK_CHECK(rels[1].relocation_symbol_name == "helper");
    TASK_CHECK(rels[1].relocation_symbol_value == c.helper_val);

    // ELF32 r_info packs the symbol in the high 24 bits and the type in the
    // low 8 bits; a 64-bit split would give symbol 0 here.
    TASK_CHECK(rels[1].relocation_symbol_index == 2);
    TASK_CHECK(rels[1].relocation_type_id == 26);
    TASK_CHECK(rels[1].relocation_info == uint64_t((2u << 8) | 26u));

    // .rela.dyn resolves through .dynsym (sh_link=5), not the first table
    // that happens to contain a record numbered 1.
    TASK_CHECK(rels[3].relocation_symbol_table_index == 5);
    TASK_CHECK(rels[3].relocation_symbol_index == 1);
    TASK_CHECK(rels[3].relocation_symbol_name == "dynsym_name");
    TASK_CHECK(rels[3].relocation_symbol_value == c.base + 0x100);
    TASK_CHECK(rels[3].relocation_target_section_index == 2);

    BuildCfg c64 = cfg_ppc64();
    std::string path64 = save_fixture("ref_ppc64.elf", build_elf(c64));
    Elf_parser parser64(path64);
    std::vector<elf_parser::relocation_t> rels64 = parser64.get_relocations();
    // ELF64 r_info: high 32 bits symbol, low 32 bits type.
    TASK_CHECK(rels64[0].relocation_info == (uint64_t(1) << 32 | 38));
    TASK_CHECK(rels64[0].relocation_symbol_index == 1);
    TASK_CHECK(rels64[0].relocation_type_id == 38);
    TASK_CHECK(rels64[0].relocation_symbol_name == "anchor");
    TASK_CHECK(rels64[3].relocation_symbol_name == "dynsym_name");
    TASK_CHECK(rels64[3].relocation_symbol_value == c64.base + 0x100);
    return 0;
}

int run_interleaved_readers() {
    BuildCfg a = cfg_i386();
    BuildCfg b = cfg_ppc64();
    BuildCfg cc = cfg_ppc32();
    BuildCfg d = cfg_x64();
    std::string pa = save_fixture("inter_a.elf", build_elf(a));
    std::string pb = save_fixture("inter_b.elf", build_elf(b));
    std::string pc = save_fixture("inter_c.elf", build_elf(cc));
    std::string pd = save_fixture("inter_d.elf", build_elf(d));

    // Alternate classes and byte orders; each instance must keep decoding its
    // own file rather than leaking state across instances.
    Elf_parser p1(pa);
    TASK_CHECK(p1.machine() == EM_386 && p1.elf_class() == ELFCLASS32);
    Elf_parser p2(pb);
    TASK_CHECK(p2.machine() == EM_PPC64 && p2.data_encoding() == ELFDATA2MSB);
    Elf_parser p3(pc);
    TASK_CHECK(p3.machine() == EM_PPC && p3.elf_class() == ELFCLASS32);
    Elf_parser p4(pd);
    TASK_CHECK(p4.machine() == EM_X86_64 && p4.data_encoding() == ELFDATA2LSB);

    TASK_CHECK(p1.get_segments()[0].segment_flags == "RE");
    TASK_CHECK(p2.get_segments()[0].segment_virtaddr == b.base);
    TASK_CHECK(p3.get_symbols()[1].symbol_visibility == "HIDDEN");
    TASK_CHECK(p4.get_relocations()[0].relocation_type == "R_X86_64_64");
    // Revisit the first parser after the others existed.
    TASK_CHECK(p1.get_relocations()[0].relocation_addend == -4);
    TASK_CHECK(p1.get_relocations()[0].relocation_type == "R_386_32");
    TASK_CHECK(p2.get_relocations()[1].relocation_symbol_name == "helper");
    return 0;
}

int run_mapping_bytes_unchanged() {
    BuildCfg c = cfg_ppc64();
    bytes original = build_elf(c);
    std::string path = save_fixture("readonly.elf", original);
    {
        Elf_parser parser(path);
        parser.get_sections();
        parser.get_segments();
        parser.get_symbols();
        parser.get_relocations();
    }
    TASK_CHECK(load_file(path) == original);
    return 0;
}

// ---- legacy: original entry points, base 64-bit LE fixture, old fields ----

int run_legacy_basic64() {
    std::string path = save_fixture("legacy_basic64.elf", sample::basic64());
    Elf_parser parser(path);
    TASK_CHECK(parser.elf_class() == ELFCLASS64);
    TASK_CHECK(parser.data_encoding() == ELFDATA2LSB);
    TASK_CHECK(parser.machine() == EM_X86_64);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(secs.size() == 6);
    TASK_CHECK(segs.size() == 1);
    TASK_CHECK(syms.size() == 2);
    TASK_CHECK(rels.size() == 1);

    // Original observable results stay intact.
    TASK_CHECK(syms[1].symbol_name == "anchor");
    TASK_CHECK(syms[1].symbol_value == 0x401002);
    TASK_CHECK(rels[0].relocation_symbol_name == "anchor");
    TASK_CHECK(segs[0].segment_type == "LOAD");
    TASK_CHECK(segs[0].segment_flags == "RE");
    TASK_CHECK(secs[2].section_name == ".text");
    TASK_CHECK(secs[2].section_size == 4);

    // Widened and newly exposed fields on the same fixture.
    TASK_CHECK(syms[1].symbol_size == 1);
    TASK_CHECK(syms[1].symbol_table_index == 4);
    TASK_CHECK(syms[1].symbol_type == "OBJECT");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(secs[4].section_size == 48);
    TASK_CHECK(secs[5].section_size == 24);
    TASK_CHECK(rels[0].relocation_offset == 0x401000);
    TASK_CHECK(rels[0].relocation_info == (uint64_t(1) << 32 | 1));
    TASK_CHECK(rels[0].relocation_type_id == 1);
    TASK_CHECK(rels[0].relocation_type == "R_X86_64_64");
    TASK_CHECK(rels[0].relocation_symbol_index == 1);
    TASK_CHECK(rels[0].relocation_symbol_table_index == 4);
    TASK_CHECK(rels[0].relocation_target_section_index == 2);
    TASK_CHECK(rels[0].relocation_has_addend == true);
    TASK_CHECK(rels[0].relocation_addend == -4);
    TASK_CHECK(rels[0].relocation_symbol_value == 0x401002);
    return 0;
}

int run_legacy_accept32_and_reject() {
    // The constructor used to refuse ELF32 outright; it must now succeed.
    bytes hdr(52, 0);
    hdr[0] = 0x7f; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
    hdr[4] = ELFCLASS32; hdr[5] = ELFDATA2LSB; hdr[6] = 1;
    put(hdr, 16, 1, 2, false);
    put(hdr, 18, EM_386, 2, false);
    put(hdr, 20, 1, 4, false);
    std::string path = save_fixture("legacy_class32.elf", hdr);
    {
        Elf_parser parser(path);
        TASK_CHECK(parser.elf_class() == ELFCLASS32);
        TASK_CHECK(parser.machine() == EM_386);
        TASK_CHECK(parser.get_sections().empty());
        TASK_CHECK(parser.get_segments().empty());
    }

    bytes bad(64, 0);
    std::string bad_path = save_fixture("legacy_badmagic.elf", bad);
    bool threw = false;
    try {
        Elf_parser parser(bad_path);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    TASK_CHECK(threw);

    bytes bad_machine = hdr;
    put(bad_machine, 18, 0x9999, 2, false);
    std::string mach_path = save_fixture("legacy_badmachine.elf", bad_machine);
    threw = false;
    try {
        Elf_parser parser(mach_path);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    TASK_CHECK(threw);

    bytes bad_class = hdr;
    bad_class[4] = 9;
    std::string class_path = save_fixture("legacy_badclass.elf", bad_class);
    threw = false;
    try {
        Elf_parser parser(class_path);
    } catch (const std::runtime_error &) {
        threw = true;
    }
    TASK_CHECK(threw);
    return 0;
}

int run_legacy_string_labels() {
    BuildCfg c = cfg_i386();
    std::string path = save_fixture("legacy_labels.elf", build_elf(c));
    Elf_parser parser(path);
    std::vector<elf_parser::section_t> secs = parser.get_sections();
    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(secs[1].section_type == "SHT_STRTAB");
    TASK_CHECK(secs[7].section_type == "SHT_RELA");
    TASK_CHECK(secs[8].section_type == "SHT_REL");
    TASK_CHECK(syms[1].symbol_type == "FUNC");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(syms[1].symbol_visibility == "HIDDEN");
    TASK_CHECK(syms[2].symbol_index == "ABS");
    TASK_CHECK(parser.get_relocations()[2].relocation_type == "R_386_PC32");
    return 0;
}

} // namespace

// External linkage on purpose: task_support/driver.c reaches these through
// the extern declarations in task_support/check.h.
const struct task_case task_cases[] = {
    {"layouts", "i386_le32", run_i386_layout},
    {"layouts", "ppc_be32", run_ppc32_layout},
    {"layouts", "x86_64_le64_highaddr", run_x64_layout},
    {"layouts", "ppc64_be64_highaddr", run_ppc64_layout},
    {"references", "sh_link_and_r_info", run_relocation_references},
    {"references", "interleaved_readers", run_interleaved_readers},
    {"references", "mapping_bytes_unchanged", run_mapping_bytes_unchanged},
    {"legacy", "basic64_results", run_legacy_basic64},
    {"legacy", "accept32_and_reject_invalid", run_legacy_accept32_and_reject},
    {"legacy", "string_labels_compatible", run_legacy_string_labels},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
