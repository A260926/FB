// Regression suites for the multi-format ELF reader: layouts (per-class,
// per-endian record layouts), references (sh_link-based table wiring) and
// legacy (the original 64-bit little-endian sample). All fixtures are
// synthetic bytes written to .task-build and parsed read-only; nothing here
// executes file contents or applies relocations.

#include "elf_parser.hpp"
#include "task_support/check.h"
#include "task_support/fixture.hh"

#include <elf.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {

using sample::bytes;

// Write v as an n-byte integer at p in the image's own byte order.
void wput(bytes &b, size_t p, uint64_t v, unsigned n, bool msb) {
    if (p + n > b.size())
        b.resize(p + n, 0);
    for (unsigned i = 0; i < n; ++i) {
        const unsigned shift = msb ? 8 * (n - 1 - i) : 8 * i;
        b[p + i] = static_cast<unsigned char>((v >> shift) & 0xff);
    }
}

// String table builder; offset 0 is always the empty string.
struct strtab {
    std::string data;
    strtab() : data(1, '\0') {}
    uint32_t add(const std::string &s) {
        const uint32_t at = static_cast<uint32_t>(data.size());
        data += s;
        data += '\0';
        return at;
    }
};

// Synthetic ELF image builder covering both classes and both byte orders.
struct image {
    bytes b;
    bool  is64;
    bool  msb;

    image(bool is64_, bool msb_, uint16_t machine, uint16_t etype)
        : is64(is64_), msb(msb_) {
        b.resize(is64 ? 64 : 52, 0);
        b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
        b[4] = is64 ? ELFCLASS64 : ELFCLASS32;
        b[5] = msb ? ELFDATA2MSB : ELFDATA2LSB;
        b[6] = 1;                                       // EV_CURRENT
        w(16, etype, 2);
        w(18, machine, 2);
        w(20, 1, 4);                                    // e_version
        w(is64 ? 52 : 40, is64 ? 64 : 52, 2);           // e_ehsize
        w(is64 ? 54 : 42, is64 ? 56 : 32, 2);           // e_phentsize
        w(is64 ? 58 : 46, is64 ? 64 : 40, 2);           // e_shentsize
    }

    void w(size_t p, uint64_t v, unsigned n) { wput(b, p, v, n, msb); }

    void pad_to(size_t p) { if (b.size() < p) b.resize(p, 0); }

    size_t reserve(size_t n, size_t align) {
        pad_to((b.size() + align - 1) / align * align);
        const size_t at = b.size();
        b.resize(at + n, 0);
        return at;
    }

    size_t append(const void *data, size_t n, size_t align) {
        const size_t at = reserve(0, align);
        const unsigned char *p = static_cast<const unsigned char*>(data);
        b.insert(b.end(), p, p + n);
        return at;
    }

    void set_phdr(uint64_t off, uint16_t num) {
        w(is64 ? 32 : 28, off, is64 ? 8 : 4);
        w(is64 ? 56 : 44, num, 2);
    }
    void set_shdr(uint64_t off, uint16_t num, uint16_t strndx) {
        w(is64 ? 40 : 32, off, is64 ? 8 : 4);
        w(is64 ? 60 : 48, num, 2);
        w(is64 ? 62 : 50, strndx, 2);
    }

    // Program header: p_flags last for ELF32, second for ELF64.
    void phdr(size_t at, uint32_t type, uint64_t off, uint64_t va, uint64_t pa,
              uint64_t filesz, uint64_t memsz, uint32_t flags, uint64_t align) {
        if (is64) {
            w(at + 0, type, 4);   w(at + 4, flags, 4);
            w(at + 8, off, 8);    w(at + 16, va, 8);   w(at + 24, pa, 8);
            w(at + 32, filesz, 8); w(at + 40, memsz, 8); w(at + 48, align, 8);
        } else {
            w(at + 0, type, 4);   w(at + 4, off, 4);   w(at + 8, va, 4);
            w(at + 12, pa, 4);    w(at + 16, filesz, 4); w(at + 20, memsz, 4);
            w(at + 24, flags, 4); w(at + 28, align, 4);
        }
    }

    void shdr(size_t at, uint32_t name, uint32_t type, uint64_t flags, uint64_t addr,
              uint64_t off, uint64_t size, uint32_t link, uint32_t info,
              uint64_t align, uint64_t entsize) {
        if (is64) {
            w(at + 0, name, 4);   w(at + 4, type, 4);    w(at + 8, flags, 8);
            w(at + 16, addr, 8);  w(at + 24, off, 8);    w(at + 32, size, 8);
            w(at + 40, link, 4);  w(at + 44, info, 4);
            w(at + 48, align, 8); w(at + 56, entsize, 8);
        } else {
            w(at + 0, name, 4);   w(at + 4, type, 4);    w(at + 8, flags, 4);
            w(at + 12, addr, 4);  w(at + 16, off, 4);    w(at + 20, size, 4);
            w(at + 24, link, 4);  w(at + 28, info, 4);
            w(at + 32, align, 4); w(at + 36, entsize, 4);
        }
    }

    // Symbol: st_info/st_other trail st_size for ELF32, lead for ELF64.
    void sym(size_t at, uint32_t name, uint64_t value, uint64_t size,
             uint8_t info, uint8_t other, uint16_t shndx) {
        if (is64) {
            w(at + 0, name, 4);   w(at + 4, info, 1);   w(at + 5, other, 1);
            w(at + 6, shndx, 2);  w(at + 8, value, 8);  w(at + 16, size, 8);
        } else {
            w(at + 0, name, 4);   w(at + 4, value, 4);  w(at + 8, size, 4);
            w(at + 12, info, 1);  w(at + 13, other, 1); w(at + 14, shndx, 2);
        }
    }

    // r_info packs the symbol index differently per class.
    uint64_t rinfo(uint32_t sym, uint32_t type) const {
        return is64 ? ((static_cast<uint64_t>(sym) << 32) | type)
                    : ((static_cast<uint64_t>(sym) << 8) | (type & 0xffu));
    }

    void rela(size_t at, uint64_t off, uint64_t info, int64_t addend) {
        if (is64) {
            w(at + 0, off, 8); w(at + 8, info, 8);
            w(at + 16, static_cast<uint64_t>(addend), 8);
        } else {
            w(at + 0, off, 4); w(at + 4, info, 4);
            w(at + 8, static_cast<uint64_t>(addend), 4); // masked to low 32 bits
        }
    }

    void rel(size_t at, uint64_t off, uint64_t info) {
        if (is64) { w(at + 0, off, 8); w(at + 8, info, 8); }
        else      { w(at + 0, off, 4); w(at + 4, info, 4); }
    }
};

// The five sections every layout fixture shares: indices 1..5 are
// .text / .symtab / .strtab / .rela.text / .shstrtab.
struct layout5 {
    uint32_t n_text, n_symtab, n_strtab, n_rela, n_shstr;
    strtab   names;
    layout5() {
        n_text   = names.add(".text");
        n_symtab = names.add(".symtab");
        n_strtab = names.add(".strtab");
        n_rela   = names.add(".rela.text");
        n_shstr  = names.add(".shstrtab");
    }
};

// 32-bit little-endian i386 (EM_386).
bytes make_i386_le() {
    image im(false, false, EM_386, 2 /* ET_EXEC */);
    const size_t ph = im.reserve(32, 4);
    im.set_phdr(ph, 1);
    im.phdr(ph, PT_LOAD, 0x100, 0x08048000, 0x08048000, 0x10, 0x18,
            PF_R | PF_X, 0x1000);

    im.pad_to(0x100);
    const unsigned char text[16] = {0xb8, 0x34, 0x12, 0x00, 0x00, 0xc3};
    const size_t text_off = im.append(text, sizeof text, 1);

    strtab symstr;
    const uint32_t n_sym = symstr.add("le32sym");
    const size_t str_off = im.append(symstr.data.data(), symstr.data.size(), 1);

    const size_t sym_off = im.reserve(2 * 16, 4);
    im.sym(sym_off + 16, n_sym, 0x08048010, 0x20,
           ELF32_ST_INFO(STB_GLOBAL, STT_FUNC), 0, 1);

    const size_t rela_off = im.reserve(12, 4);
    im.rela(rela_off, 0x08048004, im.rinfo(1, R_386_PC32), 8);

    layout5 L;
    const size_t shstr_off = im.append(L.names.data.data(), L.names.data.size(), 1);

    const size_t sh_off = im.reserve(6 * 40, 4);
    im.shdr(sh_off + 1 * 40, L.n_text,   SHT_PROGBITS, 0x6, 0x08048000, text_off, 16, 0, 0, 4, 0);
    im.shdr(sh_off + 2 * 40, L.n_symtab, SHT_SYMTAB,   0, 0, sym_off, 2 * 16, 3, 1, 4, 16);
    im.shdr(sh_off + 3 * 40, L.n_strtab, SHT_STRTAB,   0, 0, str_off, symstr.data.size(), 0, 0, 1, 0);
    im.shdr(sh_off + 4 * 40, L.n_rela,   SHT_RELA,     0, 0, rela_off, 12, 2, 1, 4, 12);
    im.shdr(sh_off + 5 * 40, L.n_shstr,  SHT_STRTAB,   0, 0, shstr_off, L.names.data.size(), 0, 0, 1, 0);
    im.set_shdr(sh_off, 6, 5);
    return im.b;
}

// 32-bit big-endian PowerPC (EM_PPC), with a negative RELA addend.
bytes make_ppc32_be() {
    image im(false, true, EM_PPC, 2 /* ET_EXEC */);
    const size_t ph = im.reserve(32, 4);
    im.set_phdr(ph, 1);
    im.phdr(ph, PT_LOAD, 0x100, 0x10000000, 0x10000000, 0x20, 0x20,
            PF_R | PF_W, 0x1000);

    im.pad_to(0x100);
    const unsigned char text[32] = {0x7c, 0x08, 0x02, 0xa6};
    const size_t text_off = im.append(text, sizeof text, 1);

    strtab symstr;
    const uint32_t n_sym = symstr.add("ppc32be");
    const size_t str_off = im.append(symstr.data.data(), symstr.data.size(), 1);

    const size_t sym_off = im.reserve(2 * 16, 4);
    im.sym(sym_off + 16, n_sym, 0x10000004, 0x1c,
           ELF32_ST_INFO(STB_GLOBAL, STT_OBJECT), 2 /* STV_HIDDEN */, 1);

    const size_t rela_off = im.reserve(2 * 12, 4);
    im.rela(rela_off + 0,  0x10000008, im.rinfo(1, R_PPC_REL32),  -16);
    im.rela(rela_off + 12, 0x1000000c, im.rinfo(0, R_PPC_ADDR32), 0x100);

    layout5 L;
    const size_t shstr_off = im.append(L.names.data.data(), L.names.data.size(), 1);

    const size_t sh_off = im.reserve(6 * 40, 4);
    im.shdr(sh_off + 1 * 40, L.n_text,   SHT_PROGBITS, 0x6, 0x10000000, text_off, 32, 0, 0, 4, 0);
    im.shdr(sh_off + 2 * 40, L.n_symtab, SHT_SYMTAB,   0, 0, sym_off, 2 * 16, 3, 1, 4, 16);
    im.shdr(sh_off + 3 * 40, L.n_strtab, SHT_STRTAB,   0, 0, str_off, symstr.data.size(), 0, 0, 1, 0);
    im.shdr(sh_off + 4 * 40, L.n_rela,   SHT_RELA,     0, 0, rela_off, 2 * 12, 2, 1, 4, 12);
    im.shdr(sh_off + 5 * 40, L.n_shstr,  SHT_STRTAB,   0, 0, shstr_off, L.names.data.size(), 0, 0, 1, 0);
    im.set_shdr(sh_off, 6, 5);
    return im.b;
}

// 64-bit big-endian PowerPC64 (EM_PPC64) with addresses above 32 bits.
bytes make_ppc64_be() {
    image im(true, true, EM_PPC64, 3 /* ET_DYN */);
    const size_t ph = im.reserve(56, 8);
    im.set_phdr(ph, 1);
    im.phdr(ph, PT_LOAD, 0x1000, 0x100000000ULL, 0x100000000ULL, 0x200, 0x200,
            PF_R | PF_X, 0x10000);

    im.pad_to(0x1000);
    const unsigned char text[64] = {0x7c, 0x08, 0x02, 0xa6};
    const size_t text_off = im.append(text, sizeof text, 1);

    strtab symstr;
    const uint32_t n_func = symstr.add("ppc64be");
    const uint32_t n_data = symstr.add("data64");
    const size_t str_off = im.append(symstr.data.data(), symstr.data.size(), 1);

    const size_t sym_off = im.reserve(3 * 24, 8);
    im.sym(sym_off + 24, n_func, 0x100000010ULL, 0x38,
           ELF64_ST_INFO(STB_GLOBAL, STT_FUNC), 0, 1);
    im.sym(sym_off + 48, n_data, 0x100001000ULL, 8,
           ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT), 0, 1);

    const size_t rela_off = im.reserve(3 * 24, 8);
    im.rela(rela_off + 0,  0x100000008ULL, im.rinfo(1, R_PPC64_ADDR64), 0x100000000LL);
    im.rela(rela_off + 24, 0x100000010ULL, im.rinfo(2, R_PPC64_REL64),  -8);
    im.rela(rela_off + 48, 0x100000018ULL, im.rinfo(1, R_PPC64_REL32),  16);

    layout5 L;
    const size_t shstr_off = im.append(L.names.data.data(), L.names.data.size(), 1);

    const size_t sh_off = im.reserve(6 * 64, 8);
    im.shdr(sh_off + 1 * 64, L.n_text,   SHT_PROGBITS, 0x6, 0x100000000ULL, text_off, 64, 0, 0, 16, 0);
    im.shdr(sh_off + 2 * 64, L.n_symtab, SHT_SYMTAB,   0, 0, sym_off, 3 * 24, 3, 1, 8, 24);
    im.shdr(sh_off + 3 * 64, L.n_strtab, SHT_STRTAB,   0, 0, str_off, symstr.data.size(), 0, 0, 1, 0);
    im.shdr(sh_off + 4 * 64, L.n_rela,   SHT_RELA,     0, 0, rela_off, 3 * 24, 2, 1, 8, 24);
    im.shdr(sh_off + 5 * 64, L.n_shstr,  SHT_STRTAB,   0, 0, shstr_off, L.names.data.size(), 0, 0, 1, 0);
    im.set_shdr(sh_off, 6, 5);
    return im.b;
}

// 64-bit little-endian x86-64 (EM_X86_64) with addresses above 32 bits.
bytes make_x86_64_le() {
    image im(true, false, EM_X86_64, 3 /* ET_DYN */);
    const size_t ph = im.reserve(56, 8);
    im.set_phdr(ph, 1);
    im.phdr(ph, PT_LOAD, 0x2000, 0x100000000ULL, 0x100000000ULL, 0x100, 0x100,
            PF_R | PF_X, 0x1000);

    im.pad_to(0x2000);
    const unsigned char text[48] = {0x48, 0x8b, 0x05};
    const size_t text_off = im.append(text, sizeof text, 1);

    strtab symstr;
    const uint32_t n_sym = symstr.add("hi64");
    const size_t str_off = im.append(symstr.data.data(), symstr.data.size(), 1);

    const size_t sym_off = im.reserve(2 * 24, 8);
    im.sym(sym_off + 24, n_sym, 0x100000200ULL, 0x10,
           ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0, 1);

    const size_t rela_off = im.reserve(2 * 24, 8);
    im.rela(rela_off + 0,  0x100000008ULL, im.rinfo(1, R_X86_64_64), -4);
    im.rela(rela_off + 24, 0x10000000cULL, im.rinfo(1, 42),           0); // unknown id

    layout5 L;
    const size_t shstr_off = im.append(L.names.data.data(), L.names.data.size(), 1);

    const size_t sh_off = im.reserve(6 * 64, 8);
    im.shdr(sh_off + 1 * 64, L.n_text,   SHT_PROGBITS, 0x6, 0x100000000ULL, text_off, 48, 0, 0, 16, 0);
    im.shdr(sh_off + 2 * 64, L.n_symtab, SHT_SYMTAB,   0, 0, sym_off, 2 * 24, 3, 1, 8, 24);
    im.shdr(sh_off + 3 * 64, L.n_strtab, SHT_STRTAB,   0, 0, str_off, symstr.data.size(), 0, 0, 1, 0);
    im.shdr(sh_off + 4 * 64, L.n_rela,   SHT_RELA,     0, 0, rela_off, 2 * 24, 2, 1, 8, 24);
    im.shdr(sh_off + 5 * 64, L.n_shstr,  SHT_STRTAB,   0, 0, shstr_off, L.names.data.size(), 0, 0, 1, 0);
    im.set_shdr(sh_off, 6, 5);
    return im.b;
}

// sh_link wiring fixture (64-bit LE x86-64, relocatable): two symbol tables
// whose strings live in sections NOT named .strtab/.dynstr, a decoy section
// that IS named .strtab, one RELA and one REL relocation section.
bytes make_refs64() {
    image im(true, false, EM_X86_64, 1 /* ET_REL */);

    im.pad_to(0x100);
    unsigned char text[32];
    for (size_t i = 0; i < sizeof text; ++i)
        text[i] = 0x90;
    // Bytes at the RELA target: the addend must come from the record, not these.
    text[0x08] = 0xef; text[0x09] = 0xbe; text[0x0a] = 0xad; text[0x0b] = 0xde;
    text[0x0c] = 0x00; text[0x0d] = 0xf0; text[0x0e] = 0x0d; text[0x0f] = 0xf0;
    // Bytes at the REL target: must never surface as an implicit addend.
    text[0x10] = 0x22; text[0x11] = 0x11; text[0x12] = 0x00; text[0x13] = 0xee;
    text[0x14] = 0xdd; text[0x15] = 0xcc; text[0x16] = 0xbb; text[0x17] = 0xaa;
    const size_t text_off = im.append(text, sizeof text, 1);

    strtab decoy; // named ".strtab" but referenced by nothing
    decoy.add("wrong1");
    decoy.add("wrong2");
    strtab real;
    const uint32_t n_alpha = real.add("alpha");
    const uint32_t n_beta  = real.add("beta");
    strtab reald;
    const uint32_t n_gamma = reald.add("gamma");
    const uint32_t n_delta = reald.add("delta");

    const size_t sym_off = im.reserve(3 * 24, 8);
    im.sym(sym_off + 24, n_alpha, 0x400100, 4, ELF64_ST_INFO(STB_GLOBAL, STT_OBJECT), 0, 1);
    im.sym(sym_off + 48, n_beta,  0x400200, 8, ELF64_ST_INFO(STB_GLOBAL, STT_FUNC),   0, 1);

    const size_t dsym_off = im.reserve(3 * 24, 8);
    im.sym(dsym_off + 24, n_gamma, 0x500000, 1, ELF64_ST_INFO(STB_WEAK,   STT_OBJECT), 0, 1);
    im.sym(dsym_off + 48, n_delta, 0x500100, 2, ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE), 0, 0);

    const size_t decoy_off = im.append(decoy.data.data(), decoy.data.size(), 1);
    const size_t real_off  = im.append(real.data.data(),  real.data.size(),  1);
    const size_t reald_off = im.append(reald.data.data(), reald.data.size(), 1);

    const size_t rela_off = im.reserve(24, 8);
    im.rela(rela_off, 0x400008, im.rinfo(2, R_X86_64_GLOB_DAT), 0x20);

    const size_t rel_off = im.reserve(16, 8);
    im.rel(rel_off, 0x400010, im.rinfo(1, R_X86_64_JUMP_SLOT));

    strtab sh;
    const uint32_t n_text   = sh.add(".text");
    const uint32_t n_symtab = sh.add(".symtab");
    const uint32_t n_dynsym = sh.add(".dynsym");
    const uint32_t n_strtab = sh.add(".strtab");
    const uint32_t n_real   = sh.add(".realstr");
    const uint32_t n_reald  = sh.add(".realdyn");
    const uint32_t n_rela   = sh.add(".rela.text");
    const uint32_t n_rel    = sh.add(".rel.text");
    const uint32_t n_shstr  = sh.add(".shstrtab");
    const size_t shstr_off = im.append(sh.data.data(), sh.data.size(), 1);

    const size_t sh_off = im.reserve(10 * 64, 8);
    im.shdr(sh_off + 1 * 64, n_text,   SHT_PROGBITS, 0x6, 0x400000, text_off, sizeof text, 0, 0, 16, 0);
    im.shdr(sh_off + 2 * 64, n_symtab, SHT_SYMTAB,   0, 0, sym_off,  3 * 24, 5, 1, 8, 24);
    im.shdr(sh_off + 3 * 64, n_dynsym, SHT_DYNSYM,   0, 0, dsym_off, 3 * 24, 6, 1, 8, 24);
    im.shdr(sh_off + 4 * 64, n_strtab, SHT_STRTAB,   0, 0, decoy_off, decoy.data.size(), 0, 0, 1, 0);
    im.shdr(sh_off + 5 * 64, n_real,   SHT_STRTAB,   0, 0, real_off,  real.data.size(),  0, 0, 1, 0);
    im.shdr(sh_off + 6 * 64, n_reald,  SHT_STRTAB,   0, 0, reald_off, reald.data.size(), 0, 0, 1, 0);
    im.shdr(sh_off + 7 * 64, n_rela,   SHT_RELA,     0, 0, rela_off, 24, 2, 1, 8, 24);
    im.shdr(sh_off + 8 * 64, n_rel,    SHT_REL,      0, 0, rel_off,  16, 3, 1, 8, 16);
    im.shdr(sh_off + 9 * 64, n_shstr,  SHT_STRTAB,   0, 0, shstr_off, sh.data.size(), 0, 0, 1, 0);
    im.set_shdr(sh_off, 10, 9);
    return im.b;
}

std::string save_fixture(const char *name, const bytes &b) {
    const std::string path = std::string(".task-build/") + name;
    sample::save(path, b);
    return path;
}

bool throws_runtime(const char *name, const bytes &b) {
    std::string path = save_fixture(name, b);
    try {
        elf_parser::Elf_parser parser(path);
    } catch (const std::runtime_error &) {
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- layouts

int case_elf32_le_i386() {
    std::string path = save_fixture("layout_i386_le.elf", make_i386_le());
    elf_parser::Elf_parser parser(path);

    TASK_CHECK(parser.elf_class() == ELFCLASS32);
    TASK_CHECK(parser.data_encoding() == ELFDATA2LSB);
    TASK_CHECK(parser.machine() == EM_386);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 6);
    TASK_CHECK(secs[1].section_name == ".text");
    TASK_CHECK(secs[1].section_type == "SHT_PROGBITS");
    TASK_CHECK(secs[1].section_type_id == SHT_PROGBITS);
    TASK_CHECK(secs[1].section_addr == 0x08048000);
    TASK_CHECK(secs[1].section_offset == 0x100);
    TASK_CHECK(secs[1].section_size == 16);
    TASK_CHECK(secs[1].section_flags == 0x6);
    TASK_CHECK(secs[1].section_addr_align == 4);
    TASK_CHECK(secs[2].section_type == "SHT_SYMTAB");
    TASK_CHECK(secs[2].section_link == 3);
    TASK_CHECK(secs[2].section_info == 1);
    TASK_CHECK(secs[2].section_ent_size == 16);
    TASK_CHECK(secs[4].section_type == "SHT_RELA");
    TASK_CHECK(secs[4].section_link == 2);
    TASK_CHECK(secs[4].section_info == 1);

    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    TASK_CHECK(segs.size() == 1);
    TASK_CHECK(segs[0].segment_type == "LOAD");
    TASK_CHECK(segs[0].segment_offset == 0x100);
    TASK_CHECK(segs[0].segment_virtaddr == 0x08048000);
    TASK_CHECK(segs[0].segment_physaddr == 0x08048000);
    TASK_CHECK(segs[0].segment_filesize == 0x10);
    TASK_CHECK(segs[0].segment_memsize == 0x18);
    TASK_CHECK(segs[0].segment_flags == "RE"); // p_flags is the last 32-bit word
    TASK_CHECK(segs[0].segment_align == 0x1000);

    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 2);
    TASK_CHECK(syms[0].symbol_num == 0);
    TASK_CHECK(syms[0].symbol_name == "");
    TASK_CHECK(syms[0].symbol_value == 0);
    TASK_CHECK(syms[0].symbol_table_index == 2);
    TASK_CHECK(syms[1].symbol_num == 1);
    TASK_CHECK(syms[1].symbol_name == "le32sym");
    TASK_CHECK(syms[1].symbol_value == 0x08048010);
    TASK_CHECK(syms[1].symbol_size == 0x20);
    TASK_CHECK(syms[1].symbol_type == "FUNC"); // st_info trails st_size in ELF32
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(syms[1].symbol_visibility == "DEFAULT");
    TASK_CHECK(syms[1].symbol_index == "1");
    TASK_CHECK(syms[1].symbol_section == ".symtab");
    TASK_CHECK(syms[1].symbol_table_index == 2);

    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 1);
    TASK_CHECK(rels[0].relocation_offset == 0x08048004);
    TASK_CHECK(rels[0].relocation_info == 0x102); // (sym << 8) | type in ELF32
    TASK_CHECK(rels[0].relocation_type_id == R_386_PC32);
    TASK_CHECK(rels[0].relocation_type == "R_386_PC32");
    TASK_CHECK(rels[0].relocation_symbol_index == 1);
    TASK_CHECK(rels[0].relocation_symbol_table_index == 2);
    TASK_CHECK(rels[0].relocation_target_section_index == 1);
    TASK_CHECK(rels[0].relocation_symbol_name == "le32sym");
    TASK_CHECK(rels[0].relocation_symbol_value == 0x08048010);
    TASK_CHECK(rels[0].relocation_has_addend);
    TASK_CHECK(rels[0].relocation_addend == 8);
    TASK_CHECK(rels[0].relocation_section_name == ".rela.text");
    return 0;
}

int case_elf32_be_ppc() {
    std::string path = save_fixture("layout_ppc32_be.elf", make_ppc32_be());
    elf_parser::Elf_parser parser(path);

    TASK_CHECK(parser.elf_class() == ELFCLASS32);
    TASK_CHECK(parser.data_encoding() == ELFDATA2MSB);
    TASK_CHECK(parser.machine() == EM_PPC);

    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    TASK_CHECK(segs.size() == 1);
    TASK_CHECK(segs[0].segment_type == "LOAD");
    TASK_CHECK(segs[0].segment_offset == 0x100);
    TASK_CHECK(segs[0].segment_virtaddr == 0x10000000);
    TASK_CHECK(segs[0].segment_filesize == 0x20);
    TASK_CHECK(segs[0].segment_memsize == 0x20);
    TASK_CHECK(segs[0].segment_flags == "RW");
    TASK_CHECK(segs[0].segment_align == 0x1000);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 6);
    TASK_CHECK(secs[1].section_addr == 0x10000000);
    TASK_CHECK(secs[1].section_offset == 0x100);
    TASK_CHECK(secs[1].section_size == 32);
    TASK_CHECK(secs[2].section_ent_size == 16);

    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 2);
    TASK_CHECK(syms[1].symbol_name == "ppc32be");
    TASK_CHECK(syms[1].symbol_value == 0x10000004);
    TASK_CHECK(syms[1].symbol_size == 0x1c);
    TASK_CHECK(syms[1].symbol_type == "OBJECT");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(syms[1].symbol_visibility == "HIDDEN"); // st_other at ELF32 offset
    TASK_CHECK(syms[1].symbol_index == "1");

    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 2);
    TASK_CHECK(rels[0].relocation_offset == 0x10000008);
    TASK_CHECK(rels[0].relocation_info == 0x11a);
    TASK_CHECK(rels[0].relocation_type_id == R_PPC_REL32);
    TASK_CHECK(rels[0].relocation_type == "R_PPC_REL32");
    TASK_CHECK(rels[0].relocation_symbol_index == 1);
    TASK_CHECK(rels[0].relocation_symbol_name == "ppc32be");
    TASK_CHECK(rels[0].relocation_symbol_value == 0x10000004);
    TASK_CHECK(rels[0].relocation_has_addend);
    TASK_CHECK(rels[0].relocation_addend == -16); // sign-extended 32-bit addend
    TASK_CHECK(rels[1].relocation_type_id == R_PPC_ADDR32);
    TASK_CHECK(rels[1].relocation_type == "R_PPC_ADDR32");
    TASK_CHECK(rels[1].relocation_symbol_index == 0); // the null symbol
    TASK_CHECK(rels[1].relocation_symbol_name == "");
    TASK_CHECK(rels[1].relocation_symbol_value == 0);
    TASK_CHECK(rels[1].relocation_addend == 0x100);
    return 0;
}

int case_elf64_be_ppc64() {
    std::string path = save_fixture("layout_ppc64_be.elf", make_ppc64_be());
    elf_parser::Elf_parser parser(path);

    TASK_CHECK(parser.elf_class() == ELFCLASS64);
    TASK_CHECK(parser.data_encoding() == ELFDATA2MSB);
    TASK_CHECK(parser.machine() == EM_PPC64);

    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    TASK_CHECK(segs.size() == 1);
    TASK_CHECK(segs[0].segment_type == "LOAD");
    TASK_CHECK(segs[0].segment_offset == 0x1000);
    TASK_CHECK(segs[0].segment_virtaddr == 0x100000000ULL); // above 32 bits
    TASK_CHECK(segs[0].segment_physaddr == 0x100000000ULL);
    TASK_CHECK(segs[0].segment_filesize == 0x200);
    TASK_CHECK(segs[0].segment_flags == "RE"); // p_flags follows p_type in ELF64
    TASK_CHECK(segs[0].segment_align == 0x10000);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 6);
    TASK_CHECK(secs[1].section_addr == 0x100000000ULL);
    TASK_CHECK(secs[1].section_offset == 0x1000);
    TASK_CHECK(secs[1].section_size == 64);
    TASK_CHECK(secs[2].section_ent_size == 24);

    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 3);
    TASK_CHECK(syms[1].symbol_name == "ppc64be");
    TASK_CHECK(syms[1].symbol_value == 0x100000010ULL);
    TASK_CHECK(syms[1].symbol_size == 0x38);
    TASK_CHECK(syms[1].symbol_type == "FUNC");
    TASK_CHECK(syms[2].symbol_name == "data64");
    TASK_CHECK(syms[2].symbol_value == 0x100001000ULL);
    TASK_CHECK(syms[2].symbol_size == 8);
    TASK_CHECK(syms[2].symbol_type == "OBJECT");

    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 3);
    TASK_CHECK(rels[0].relocation_offset == 0x100000008ULL);
    TASK_CHECK(rels[0].relocation_type_id == R_PPC64_ADDR64);
    TASK_CHECK(rels[0].relocation_type == "R_PPC64_ADDR64");
    TASK_CHECK(rels[0].relocation_symbol_name == "ppc64be");
    TASK_CHECK(rels[0].relocation_symbol_value == 0x100000010ULL);
    TASK_CHECK(rels[0].relocation_has_addend);
    TASK_CHECK(rels[0].relocation_addend == 0x100000000LL); // addend above 32 bits
    TASK_CHECK(rels[1].relocation_type_id == R_PPC64_REL64);
    TASK_CHECK(rels[1].relocation_type == "R_PPC64_REL64");
    TASK_CHECK(rels[1].relocation_symbol_name == "data64");
    TASK_CHECK(rels[1].relocation_addend == -8);
    TASK_CHECK(rels[2].relocation_type_id == R_PPC64_REL32);
    TASK_CHECK(rels[2].relocation_type == "R_PPC64_REL32");
    TASK_CHECK(rels[2].relocation_addend == 16);
    return 0;
}

int case_elf64_le_x86_64() {
    std::string path = save_fixture("layout_x86_64_le.elf", make_x86_64_le());
    elf_parser::Elf_parser parser(path);

    TASK_CHECK(parser.elf_class() == ELFCLASS64);
    TASK_CHECK(parser.data_encoding() == ELFDATA2LSB);
    TASK_CHECK(parser.machine() == EM_X86_64);

    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    TASK_CHECK(segs.size() == 1);
    TASK_CHECK(segs[0].segment_virtaddr == 0x100000000ULL);
    TASK_CHECK(segs[0].segment_flags == "RE");

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 6);
    TASK_CHECK(secs[1].section_addr == 0x100000000ULL);

    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 2);
    TASK_CHECK(syms[1].symbol_name == "hi64");
    TASK_CHECK(syms[1].symbol_value == 0x100000200ULL);
    TASK_CHECK(syms[1].symbol_type == "NOTYPE");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");

    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 2);
    TASK_CHECK(rels[0].relocation_type_id == R_X86_64_64);
    TASK_CHECK(rels[0].relocation_type == "R_X86_64_64"); // id 1 on x86-64
    TASK_CHECK(rels[0].relocation_addend == -4);
    TASK_CHECK(rels[1].relocation_type_id == 42); // unknown id keeps its number
    TASK_CHECK(rels[1].relocation_type == "OTHERS");
    TASK_CHECK(rels[1].relocation_symbol_name == "hi64");
    return 0;
}

int case_interleaved_readers() {
    std::string p32le = save_fixture("mix_i386_le.elf",  make_i386_le());
    std::string p32be = save_fixture("mix_ppc32_be.elf", make_ppc32_be());
    std::string p64be = save_fixture("mix_ppc64_be.elf", make_ppc64_be());
    std::string p64le = save_fixture("mix_x86_64_le.elf", make_x86_64_le());
    elf_parser::Elf_parser a(p32le), b(p32be), c(p64be), d(p64le);

    // Alternate calls across objects of different class and byte order.
    TASK_CHECK(a.get_segments()[0].segment_flags == "RE");
    TASK_CHECK(b.get_relocations()[0].relocation_addend == -16);
    TASK_CHECK(c.get_symbols()[1].symbol_value == 0x100000010ULL);
    TASK_CHECK(d.get_relocations()[0].relocation_type == "R_X86_64_64");

    TASK_CHECK(a.get_symbols()[1].symbol_name == "le32sym");
    TASK_CHECK(b.get_sections()[1].section_addr == 0x10000000);
    TASK_CHECK(c.get_segments()[0].segment_virtaddr == 0x100000000ULL);
    TASK_CHECK(d.get_sections()[1].section_addr == 0x100000000ULL);

    TASK_CHECK(a.machine() == EM_386   && b.machine() == EM_PPC);
    TASK_CHECK(c.machine() == EM_PPC64 && d.machine() == EM_X86_64);
    TASK_CHECK(a.elf_class() == ELFCLASS32 && c.elf_class() == ELFCLASS64);
    TASK_CHECK(a.data_encoding() == ELFDATA2LSB && b.data_encoding() == ELFDATA2MSB);
    return 0;
}

int case_rejects_unsupported() {
    const bytes good = sample::basic64();

    bytes bad_magic = good;
    bad_magic[0] = 0;
    TASK_CHECK(throws_runtime("reject_magic.elf", bad_magic));

    bytes bad_class = good;
    bad_class[EI_CLASS] = 3;
    TASK_CHECK(throws_runtime("reject_class.elf", bad_class));

    bytes bad_data = good;
    bad_data[EI_DATA] = 3;
    TASK_CHECK(throws_runtime("reject_data.elf", bad_data));

    bytes bad_machine = good;
    sample::put(bad_machine, 18, EM_ARM, 2);
    TASK_CHECK(throws_runtime("reject_machine.elf", bad_machine));

    // The unmodified sample still parses and reports its metadata.
    std::string path = save_fixture("reject_control.elf", good);
    elf_parser::Elf_parser parser(path);
    TASK_CHECK(parser.machine() == EM_X86_64);
    return 0;
}

// ------------------------------------------------------------- references

int case_symbol_strings_via_sh_link() {
    std::string path = save_fixture("refs_symbols.elf", make_refs64());
    elf_parser::Elf_parser parser(path);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 10);
    TASK_CHECK(secs[4].section_name == ".strtab");  // decoy, linked by nothing
    TASK_CHECK(secs[2].section_link == 5);          // .symtab  -> .realstr
    TASK_CHECK(secs[3].section_link == 6);          // .dynsym  -> .realdyn
    TASK_CHECK(secs[3].section_type == "SHT_DYNSYM");
    TASK_CHECK(secs[3].section_type_id == SHT_DYNSYM);

    // Physical table order and record order are preserved, null symbols
    // included; names come through each table's own sh_link, so the decoy
    // ".strtab" contents must never appear.
    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 6);

    TASK_CHECK(syms[0].symbol_num == 0);
    TASK_CHECK(syms[0].symbol_name == "");
    TASK_CHECK(syms[0].symbol_table_index == 2);
    TASK_CHECK(syms[0].symbol_section == ".symtab");

    TASK_CHECK(syms[1].symbol_num == 1);
    TASK_CHECK(syms[1].symbol_name == "alpha");
    TASK_CHECK(syms[1].symbol_value == 0x400100);
    TASK_CHECK(syms[1].symbol_size == 4);
    TASK_CHECK(syms[1].symbol_type == "OBJECT");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(syms[1].symbol_index == "1");
    TASK_CHECK(syms[1].symbol_table_index == 2);

    TASK_CHECK(syms[2].symbol_num == 2);
    TASK_CHECK(syms[2].symbol_name == "beta");
    TASK_CHECK(syms[2].symbol_value == 0x400200);
    TASK_CHECK(syms[2].symbol_type == "FUNC");
    TASK_CHECK(syms[2].symbol_table_index == 2);

    TASK_CHECK(syms[3].symbol_num == 0);
    TASK_CHECK(syms[3].symbol_name == "");
    TASK_CHECK(syms[3].symbol_table_index == 3);
    TASK_CHECK(syms[3].symbol_section == ".dynsym");

    TASK_CHECK(syms[4].symbol_num == 1);
    TASK_CHECK(syms[4].symbol_name == "gamma");
    TASK_CHECK(syms[4].symbol_value == 0x500000);
    TASK_CHECK(syms[4].symbol_bind == "WEAK");
    TASK_CHECK(syms[4].symbol_table_index == 3);

    TASK_CHECK(syms[5].symbol_num == 2);
    TASK_CHECK(syms[5].symbol_name == "delta");
    TASK_CHECK(syms[5].symbol_value == 0x500100);
    TASK_CHECK(syms[5].symbol_type == "NOTYPE");
    TASK_CHECK(syms[5].symbol_index == "UND");
    TASK_CHECK(syms[5].symbol_table_index == 3);
    return 0;
}

int case_relocations_via_sh_link() {
    std::string path = save_fixture("refs_relocations.elf", make_refs64());
    elf_parser::Elf_parser parser(path);

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs[7].section_type == "SHT_RELA");
    TASK_CHECK(secs[7].section_type_id == SHT_RELA);
    TASK_CHECK(secs[7].section_link == 2);
    TASK_CHECK(secs[7].section_info == 1);
    TASK_CHECK(secs[8].section_type == "SHT_REL");
    TASK_CHECK(secs[8].section_type_id == SHT_REL);
    TASK_CHECK(secs[8].section_link == 3);
    TASK_CHECK(secs[8].section_info == 1);

    // Relocations come out in physical section order, original record order.
    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 2);

    // .rela.text links to .symtab: symbol 2 there is "beta".
    TASK_CHECK(rels[0].relocation_section_name == ".rela.text");
    TASK_CHECK(rels[0].relocation_offset == 0x400008);
    TASK_CHECK(rels[0].relocation_info == ((uint64_t(2) << 32) | R_X86_64_GLOB_DAT));
    TASK_CHECK(rels[0].relocation_type_id == R_X86_64_GLOB_DAT);
    TASK_CHECK(rels[0].relocation_type == "R_X86_64_GLOB_DAT");
    TASK_CHECK(rels[0].relocation_symbol_table_index == 2);
    TASK_CHECK(rels[0].relocation_symbol_index == 2);
    TASK_CHECK(rels[0].relocation_target_section_index == 1);
    TASK_CHECK(rels[0].relocation_symbol_name == "beta");
    TASK_CHECK(rels[0].relocation_symbol_value == 0x400200);
    TASK_CHECK(rels[0].relocation_has_addend);
    TASK_CHECK(rels[0].relocation_addend == 0x20); // from the record, not the target

    // .rel.text links to .dynsym: symbol 1 there is "gamma", not the
    // first symbol numbered 1 anywhere ("alpha" in .symtab).
    TASK_CHECK(rels[1].relocation_section_name == ".rel.text");
    TASK_CHECK(rels[1].relocation_offset == 0x400010);
    TASK_CHECK(rels[1].relocation_info == ((uint64_t(1) << 32) | R_X86_64_JUMP_SLOT));
    TASK_CHECK(rels[1].relocation_type_id == R_X86_64_JUMP_SLOT);
    TASK_CHECK(rels[1].relocation_type == "R_X86_64_JUMP_SLOT");
    TASK_CHECK(rels[1].relocation_symbol_table_index == 3);
    TASK_CHECK(rels[1].relocation_symbol_index == 1);
    TASK_CHECK(rels[1].relocation_target_section_index == 1);
    TASK_CHECK(rels[1].relocation_symbol_name == "gamma");
    TASK_CHECK(rels[1].relocation_symbol_value == 0x500000);
    // REL has no explicit addend; the 0xaa... bytes at the target stay unread.
    TASK_CHECK(!rels[1].relocation_has_addend);
    TASK_CHECK(rels[1].relocation_addend == 0);
    return 0;
}

// ---------------------------------------------------------------- legacy

int case_legacy_basic64() {
    std::string path = save_fixture("legacy_basic64.elf", sample::basic64());
    elf_parser::Elf_parser parser(path);

    TASK_CHECK(parser.elf_class() == ELFCLASS64);
    TASK_CHECK(parser.data_encoding() == ELFDATA2LSB);
    TASK_CHECK(parser.machine() == EM_X86_64);

    // The mapped bytes are exposed unmodified.
    const uint8_t *map = parser.get_memory_map();
    TASK_CHECK(map != 0);
    TASK_CHECK(map[0] == 0x7f && map[1] == 'E' && map[2] == 'L' && map[3] == 'F');

    std::vector<elf_parser::section_t> secs = parser.get_sections();
    TASK_CHECK(secs.size() == 6);
    TASK_CHECK(secs[0].section_type == "SHT_NULL");
    TASK_CHECK(secs[1].section_name == ".shstrtab");
    TASK_CHECK(secs[2].section_name == ".text");
    TASK_CHECK(secs[2].section_type == "SHT_PROGBITS");
    TASK_CHECK(secs[2].section_type_id == SHT_PROGBITS);
    TASK_CHECK(secs[2].section_addr == 0x401000);
    TASK_CHECK(secs[2].section_offset == 128);
    TASK_CHECK(secs[2].section_size == 4);
    TASK_CHECK(secs[2].section_flags == 0x6);
    TASK_CHECK(secs[4].section_name == ".symtab");
    TASK_CHECK(secs[4].section_type_id == SHT_SYMTAB);
    TASK_CHECK(secs[4].section_link == 3);
    TASK_CHECK(secs[4].section_info == 1);
    TASK_CHECK(secs[4].section_ent_size == 24);
    TASK_CHECK(secs[5].section_name == ".rela.text");
    TASK_CHECK(secs[5].section_type_id == SHT_RELA);
    TASK_CHECK(secs[5].section_link == 4);
    TASK_CHECK(secs[5].section_info == 2);

    std::vector<elf_parser::segment_t> segs = parser.get_segments();
    TASK_CHECK(segs.size() == 1);
    TASK_CHECK(segs[0].segment_type == "LOAD");
    TASK_CHECK(segs[0].segment_offset == 128);
    TASK_CHECK(segs[0].segment_virtaddr == 0x401000);
    TASK_CHECK(segs[0].segment_physaddr == 0x401000);
    TASK_CHECK(segs[0].segment_filesize == 4);
    TASK_CHECK(segs[0].segment_memsize == 32);
    TASK_CHECK(segs[0].segment_flags == "RE");
    TASK_CHECK(segs[0].segment_align == 1);

    std::vector<elf_parser::symbol_t> syms = parser.get_symbols();
    TASK_CHECK(syms.size() == 2);
    TASK_CHECK(syms[0].symbol_num == 0);
    TASK_CHECK(syms[0].symbol_name == "");
    TASK_CHECK(syms[0].symbol_value == 0);
    TASK_CHECK(syms[0].symbol_index == "UND");
    TASK_CHECK(syms[0].symbol_table_index == 4);
    TASK_CHECK(syms[1].symbol_num == 1);
    TASK_CHECK(syms[1].symbol_name == "anchor");
    TASK_CHECK(syms[1].symbol_value == 0x401002);
    TASK_CHECK(syms[1].symbol_size == 1);
    TASK_CHECK(syms[1].symbol_type == "OBJECT");
    TASK_CHECK(syms[1].symbol_bind == "GLOBAL");
    TASK_CHECK(syms[1].symbol_visibility == "DEFAULT");
    TASK_CHECK(syms[1].symbol_index == "2");
    TASK_CHECK(syms[1].symbol_section == ".symtab");
    TASK_CHECK(syms[1].symbol_table_index == 4);

    std::vector<elf_parser::relocation_t> rels = parser.get_relocations();
    TASK_CHECK(rels.size() == 1);
    TASK_CHECK(rels[0].relocation_offset == 0x401000);
    TASK_CHECK(rels[0].relocation_info == ((uint64_t(1) << 32) | 1));
    TASK_CHECK(rels[0].relocation_type_id == 1);
    TASK_CHECK(rels[0].relocation_type == "R_X86_64_64");
    TASK_CHECK(rels[0].relocation_symbol_index == 1);
    TASK_CHECK(rels[0].relocation_symbol_table_index == 4);
    TASK_CHECK(rels[0].relocation_target_section_index == 2);
    TASK_CHECK(rels[0].relocation_symbol_name == "anchor");
    TASK_CHECK(rels[0].relocation_symbol_value == 0x401002);
    TASK_CHECK(rels[0].relocation_has_addend);
    TASK_CHECK(rels[0].relocation_addend == -4);
    TASK_CHECK(rels[0].relocation_section_name == ".rela.text");
    return 0;
}

int case_legacy_class32_header() {
    // The baseline class32-probe bytes: a minimal ELF32 header with no
    // sections or program headers must construct and report metadata.
    bytes b(52, 0);
    b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F';
    b[4] = ELFCLASS32; b[5] = ELFDATA2LSB; b[6] = 1;
    sample::put(b, 16, 1, 2);
    sample::put(b, 18, EM_386, 2);
    sample::put(b, 20, 1, 4);
    sample::put(b, 40, 52, 2);
    sample::put(b, 42, 32, 2);
    sample::put(b, 46, 40, 2);

    std::string path = save_fixture("legacy_class32.elf", b);
    elf_parser::Elf_parser parser(path);
    TASK_CHECK(parser.elf_class() == ELFCLASS32);
    TASK_CHECK(parser.data_encoding() == ELFDATA2LSB);
    TASK_CHECK(parser.machine() == EM_386);
    TASK_CHECK(parser.get_sections().empty());
    TASK_CHECK(parser.get_segments().empty());
    TASK_CHECK(parser.get_symbols().empty());
    TASK_CHECK(parser.get_relocations().empty());
    return 0;
}

} // namespace

const struct task_case task_cases[] = {
    {"layouts", "elf32_le_i386", case_elf32_le_i386},
    {"layouts", "elf32_be_ppc", case_elf32_be_ppc},
    {"layouts", "elf64_be_ppc64", case_elf64_be_ppc64},
    {"layouts", "elf64_le_x86_64", case_elf64_le_x86_64},
    {"layouts", "interleaved_readers", case_interleaved_readers},
    {"layouts", "rejects_unsupported", case_rejects_unsupported},
    {"references", "symbol_strings_via_sh_link", case_symbol_strings_via_sh_link},
    {"references", "relocations_via_sh_link", case_relocations_via_sh_link},
    {"legacy", "basic64_fixture", case_legacy_basic64},
    {"legacy", "class32_header", case_legacy_class32_header},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
