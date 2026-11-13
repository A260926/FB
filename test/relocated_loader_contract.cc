// Contract tests for dwarf::elf::create_relocated_loader.
//
// Every ELF image used here is assembled byte by byte in C++; no host
// toolchain object files are involved.

#include "../task_support/check.h"

#include "elf++.hh"
#include "dwarf++.hh"
#include "elf_reloc.hh"

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace std;

namespace
{
        // ---------------- Little-endian field writers ----------------

        void
        put16(vector<unsigned char> &b, size_t o, uint64_t v)
        {
                for (unsigned i = 0; i < 2; i++)
                        b[o + i] = (unsigned char)(v >> (8 * i));
        }

        void
        put32(vector<unsigned char> &b, size_t o, uint64_t v)
        {
                for (unsigned i = 0; i < 4; i++)
                        b[o + i] = (unsigned char)(v >> (8 * i));
        }

        void
        put64(vector<unsigned char> &b, size_t o, uint64_t v)
        {
                for (unsigned i = 0; i < 8; i++)
                        b[o + i] = (unsigned char)(v >> (8 * i));
        }

        uint32_t
        get32(const unsigned char *p)
        {
                return  (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        }

        uint64_t
        get64(const unsigned char *p)
        {
                uint64_t v = 0;
                for (unsigned i = 0; i < 8; i++)
                        v |= (uint64_t)p[i] << (8 * i);
                return v;
        }

        // ---------------- In-memory ELF loader ----------------

        struct mem_loader : public elf::loader
        {
                vector<unsigned char> bytes;

                const void *
                load(off_t off, size_t n) override
                {
                        if (off < 0 || (size_t)off > bytes.size() ||
                            n > bytes.size() - (size_t)off)
                                throw range_error("mem_loader bounds");
                        return bytes.data() + off;
                }
        };

        // ---------------- Minimal ELF64/LE ET_REL builder ----------------

        const uint32_t SHT_PROGBITS_ = 1;
        const uint32_t SHT_SYMTAB_   = 2;
        const uint32_t SHT_STRTAB_   = 3;
        const uint32_t SHT_RELA_     = 4;
        const uint32_t SHT_REL_      = 9;

        const uint64_t SHF_ALLOC_     = 0x2;
        const uint64_t SHF_EXECINSTR_ = 0x4;
        const uint64_t SHF_COMPRESSED_ = 0x800;

        struct sym_def
        {
                uint32_t name_off;
                uint8_t  info;
                uint16_t shndx;
                uint64_t value;
                uint64_t size;
        };

        struct rela_def
        {
                uint64_t offset;
                uint32_t sym;
                uint32_t type;
                int64_t  addend;
        };

        struct section_def
        {
                string name;
                uint32_t type;
                uint64_t flags;
                uint32_t link;
                uint32_t info;
                uint64_t addralign;
                uint64_t entsize;
                vector<unsigned char> data;
        };

        class elf_builder
        {
        public:
                uint16_t machine;
                uint16_t type;
                uint8_t  ei_class;
                uint8_t  ei_data;

                elf_builder()
                        : machine(62), type(1), ei_class(2), ei_data(1) { }

                unsigned
                add(const string &name, uint32_t type, uint64_t flags,
                    vector<unsigned char> data,
                    uint64_t entsize = 0, uint64_t addralign = 1,
                    uint32_t link = 0, uint32_t info = 0)
                {
                        section_def s;
                        s.name = name;
                        s.type = type;
                        s.flags = flags;
                        s.link = link;
                        s.info = info;
                        s.entsize = entsize;
                        s.addralign = addralign;
                        s.data = std::move(data);
                        secs.push_back(std::move(s));
                        return (unsigned)secs.size(); // physical index (0 is null)
                }

                unsigned
                add_strtab(const string &name, const vector<char> &strings)
                {
                        vector<unsigned char> d(strings.begin(), strings.end());
                        return add(name, SHT_STRTAB_, 0, std::move(d), 0, 1);
                }

                unsigned
                add_symtab(const string &name, unsigned strtab_idx,
                           const vector<sym_def> &syms)
                {
                        vector<unsigned char> d;
                        for (auto &s : syms) {
                                size_t base = d.size();
                                d.resize(base + 24, 0);
                                put32(d, base + 0, s.name_off);
                                d[base + 4] = s.info;
                                d[base + 5] = 0;
                                put16(d, base + 6, s.shndx);
                                put64(d, base + 8, s.value);
                                put64(d, base + 16, s.size);
                        }
                        return add(name, SHT_SYMTAB_, 0, std::move(d), 24, 8,
                                   strtab_idx, 1);
                }

                unsigned
                add_rela(const string &name, unsigned symtab_idx,
                         unsigned target_idx, const vector<rela_def> &relas,
                         uint32_t rela_type = SHT_RELA_,
                         size_t force_size = 0)
                {
                        vector<unsigned char> d;
                        if (rela_type == SHT_RELA_) {
                                for (auto &r : relas) {
                                        size_t base = d.size();
                                        d.resize(base + 24, 0);
                                        put64(d, base + 0, r.offset);
                                        put64(d, base + 8,
                                              ((uint64_t)r.sym << 32) | r.type);
                                        put64(d, base + 16, (uint64_t)r.addend);
                                }
                        } else {
                                // SHT_REL entries are 16 bytes; content does
                                // not matter beyond reaching the rejection.
                                for (auto &r : relas) {
                                        size_t base = d.size();
                                        d.resize(base + 16, 0);
                                        put64(d, base + 0, r.offset);
                                        put64(d, base + 8,
                                              ((uint64_t)r.sym << 32) | r.type);
                                }
                        }
                        if (force_size)
                                d.resize(force_size, 0);
                        return add(name, rela_type, 0, std::move(d),
                                   rela_type == SHT_RELA_ ? 24 : 16, 8,
                                   symtab_idx, target_idx);
                }

                shared_ptr<mem_loader>
                build()
                {
                        auto m = make_shared<mem_loader>();
                        auto &out = m->bytes;

                        // Build the section name string table.
                        vector<char> shstr;
                        shstr.push_back('\0');
                        vector<uint32_t> name_offs;
                        for (auto &s : secs) {
                                name_offs.push_back((uint32_t)shstr.size());
                                shstr.insert(shstr.end(), s.name.begin(),
                                             s.name.end());
                                shstr.push_back('\0');
                        }
                        uint32_t shstr_name = (uint32_t)shstr.size();
                        shstr.insert(shstr.end(), {'.', 's', 'h', 's', 't',
                                                   'r', 't', 'a', 'b', '\0'});

                        size_t shnum = secs.size() + 2; // null + sections + shstr
                        size_t shstrndx = shnum - 1;

                        // Lay out section payloads after the 64-byte header.
                        size_t off = 64;
                        vector<size_t> offsets, sizes;
                        for (auto &s : secs) {
                                size_t a = (size_t)s.addralign;
                                if (a > 8) a = 8;
                                if (a) off = (off + a - 1) & ~(a - 1);
                                offsets.push_back(off);
                                sizes.push_back(s.data.size());
                                off += s.data.size();
                        }
                        size_t shstr_align = 1;
                        off = (off + shstr_align - 1) & ~(shstr_align - 1);
                        size_t shstr_off = off;
                        off += shstr.size();

                        off = (off + 7) & ~(size_t)7;
                        size_t shoff = off;

                        out.assign(shoff + shnum * 64, 0);

                        // e_ident
                        out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
                        out[4] = ei_class;
                        out[5] = ei_data;
                        out[6] = 1;
                        put16(out, 16, type);
                        put16(out, 18, machine);
                        put32(out, 20, 1);
                        put64(out, 32, 0);   // entry
                        put64(out, 40, shoff);
                        put16(out, 52, 64);  // ehsize
                        put16(out, 54, 0);   // phentsize
                        put16(out, 56, 0);   // phnum
                        put16(out, 58, 64);  // shentsize
                        put16(out, 60, (uint64_t)shnum);
                        put16(out, 62, (uint64_t)shstrndx);

                        // Payloads
                        for (size_t i = 0; i < secs.size(); i++)
                                memcpy(out.data() + offsets[i],
                                       secs[i].data.data(),
                                       secs[i].data.size());
                        memcpy(out.data() + shstr_off, shstr.data(),
                               shstr.size());

                        // Section headers: 0 = null
                        for (size_t i = 0; i < secs.size(); i++) {
                                size_t h = shoff + (i + 1) * 64;
                                auto &s = secs[i];
                                put32(out, h + 0, name_offs[i]);
                                put32(out, h + 4, s.type);
                                put64(out, h + 8, s.flags);
                                put64(out, h + 16, 0); // addr
                                put64(out, h + 24, offsets[i]);
                                put64(out, h + 32, sizes[i]);
                                put32(out, h + 40, s.link);
                                put32(out, h + 44, s.info);
                                put64(out, h + 48, s.addralign);
                                put64(out, h + 56, s.entsize);
                        }
                        size_t h = shoff + shstrndx * 64;
                        put32(out, h + 0, shstr_name);
                        put32(out, h + 4, SHT_STRTAB_);
                        put64(out, h + 24, shstr_off);
                        put64(out, h + 32, shstr.size());
                        put64(out, h + 48, 1);

                        return m;
                }

        private:
                vector<section_def> secs;
        };

        uint8_t
        sym_info(unsigned bind, unsigned stt)
        {
                return (uint8_t)((bind << 4) | (stt & 0xf));
        }

        // String table builder: tracks offsets of inserted strings.
        struct strtab_builder
        {
                vector<char> bytes;
                map<string, uint32_t> offs;

                strtab_builder() { bytes.push_back('\0'); }

                uint32_t
                add(const string &s)
                {
                        auto it = offs.find(s);
                        if (it != offs.end())
                                return it->second;
                        uint32_t o = (uint32_t)bytes.size();
                        bytes.insert(bytes.end(), s.begin(), s.end());
                        bytes.push_back('\0');
                        offs[s] = o;
                        return o;
                }
        };

        bool
        throws(const function<void()> &fn)
        {
                try {
                        fn();
                } catch (const exception &) {
                        return true;
                }
                return false;
        }

        shared_ptr<dwarf::loader>
        make_loader(const shared_ptr<mem_loader> &m,
                    const map<unsigned, uint64_t> &bases)
        {
                elf::elf f(m);
                return dwarf::elf::create_relocated_loader(f, bases);
        }

        // Relocation type numbers (AMD64 ABI).
        const uint32_t R_NONE = 0;
        const uint32_t R_64   = 1;
        const uint32_t R_PC32 = 2;
        const uint32_t R_32   = 10;
        const uint32_t R_32S  = 11;
}

// =================================================================
// contract_relocations: arithmetic, boundaries, diagnostics and the
// lazy/isolation rules.
// =================================================================

int
contract_relocations()
{
        // -------- One fully populated fixture --------
        elf_builder b;
        const unsigned TEXT = b.add(".text", SHT_PROGBITS_,
                                    SHF_ALLOC_ | SHF_EXECINSTR_,
                                    vector<unsigned char>(32, 0), 0, 8);
        vector<unsigned char> info(64, 0xCC);
        for (unsigned i = 32; i < 36; i++)
                info[i] = 0xAB; // NONE target keeps these bytes
        const unsigned DBG = b.add(".debug_info", SHT_PROGBITS_, 0, info);

        strtab_builder stb;
        uint32_t n_foo = stb.add("foo");
        uint32_t n_abs = stb.add("absval");
        uint32_t n_undef = stb.add("undefsym");
        uint32_t n_common = stb.add("commonsym");
        const unsigned STR = b.add_strtab(".strtab", stb.bytes);

        const vector<sym_def> syms = {
                // 0: null
                {0, 0, 0, 0, 0},
                // 1: STT_SECTION for .text
                {0, sym_info(0, 3), (uint16_t)TEXT, 0, 0},
                // 2 and 3: same name "foo", distinct identities
                {n_foo, sym_info(1, 2), (uint16_t)TEXT, 0x10, 0},
                {n_foo, sym_info(1, 2), (uint16_t)TEXT, 0x20, 0},
                // 4: STT_SECTION for .debug_info
                {0, sym_info(0, 3), (uint16_t)DBG, 0, 0},
                // 5: SHN_ABS
                {n_abs, sym_info(1, 0), 0xfff1, 0x4000, 0},
                // 6: empty-name defined symbol
                {0, sym_info(1, 0), (uint16_t)TEXT, 5, 0},
                // 7: undefined
                {n_undef, sym_info(1, 0), 0, 0, 0},
                // 8: COMMON
                {n_common, sym_info(1, 0), 0xfff2, 0x100, 0x100},
        };
        const unsigned SYM = b.add_symtab(".symtab", STR, syms);

        // Deliberately non-standard names: the loader must find targets
        // through sh_info, not name conventions.
        const unsigned RELA1 = b.add_rela(".rela.debug_info.part_one",
                                          SYM, DBG, {
                {0,  1, R_64,   7},
                {8,  2, R_32,   2},
                {16, 3, R_32S, -4},
                {24, 1, R_PC32, 100},
                {32, 0, R_NONE, 0},
                {36, 5, R_32,   3},
        });
        const unsigned RELA2 = b.add_rela(".rela.debug_info.part_two",
                                          SYM, DBG, {
                {40, 6, R_64, 1},
        });
        (void)RELA1; (void)RELA2;

        auto mem = b.build();
        const uint64_t B = 0x10000; // non-default .text base
        map<unsigned, uint64_t> bases;
        bases[TEXT] = B;
        bases[DBG] = 0;

        auto ld = make_loader(mem, bases);
        size_t sz = 0;
        const unsigned char *p =
                (const unsigned char *)ld->load(dwarf::section_type::info, &sz);
        TASK_CHECK(p != nullptr);
        TASK_CHECK(sz == 64);

        // R_X86_64_64: S + A = B + 0 + 7
        TASK_CHECK(get64(p + 0) == B + 7);
        // R_X86_64_32: foo#1 value 0x10
        TASK_CHECK(get32(p + 8) == (uint32_t)(B + 0x12));
        // R_X86_64_32S: same-name foo#2 value 0x20 -> different result
        TASK_CHECK(get32(p + 16) == (uint32_t)(int32_t)(B + 0x20 - 4));
        TASK_CHECK(get32(p + 8) != get32(p + 16));
        // R_X86_64_PC32: S + A - P, P = debug base 0 + 24
        TASK_CHECK(get32(p + 24) == (uint32_t)(int32_t)(B + 100 - 24));
        // R_X86_64_NONE leaves the field untouched
        TASK_CHECK(p[32] == 0xAB && p[33] == 0xAB && p[34] == 0xAB &&
                   p[35] == 0xAB);
        // SHN_ABS: S is st_value directly
        TASK_CHECK(get32(p + 36) == (uint32_t)0x4003);
        // Empty-name defined symbol resolves like any other symbol
        TASK_CHECK(get64(p + 40) == B + 5 + 1);
        // Untouched bytes keep their original fill
        TASK_CHECK(p[48] == 0xCC);

        // Original ELF image must be unchanged.
        elf::elf raw(mem);
        const unsigned char *rawp =
                (const unsigned char *)raw.get_section(DBG).data();
        TASK_CHECK(get64(rawp + 0) == 0xCCCCCCCCCCCCCCCCull);
        TASK_CHECK(get32(rawp + 8) == 0xCCCCCCCC);

        // Missing base for a participating symbol section.
        {
                map<unsigned, uint64_t> nb;
                nb[DBG] = 0;
                auto l2 = make_loader(mem, nb);
                TASK_CHECK(throws([&]() {
                        size_t s; l2->load(dwarf::section_type::info, &s);
                }));
        }
        // Missing base for the PC32 target section.
        {
                map<unsigned, uint64_t> nb;
                nb[TEXT] = B;
                auto l2 = make_loader(mem, nb);
                TASK_CHECK(throws([&]() {
                        size_t s; l2->load(dwarf::section_type::info, &s);
                }));
        }
        // Undefined, COMMON and extended section indices are rejected
        // with table/entry positions in the diagnostic.
        for (uint32_t bad_sym : {7u, 8u}) {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(1, 0),
                                       (uint16_t)(bad_sym == 8 ? 0xfff2 : 0),
                                       0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d,
                            {{0, 1, R_64, 0}});
                (void)t;
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(1, 0), 0xff00, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d, {{0, 1, R_64, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // Unknown relocation type, symbol index out of range, and a
        // write that runs past the target.
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d, {{0, 1, 999, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d, {{0, 42, R_64, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d, {{16, 1, R_32, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // Truncated trailing RELA entry and bad sh_link.
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d, {{0, 1, R_64, 0}},
                            SHT_RELA_, 25);
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                eb.add_strtab(".strtab", sb.bytes);
                // A rela table whose sh_link points at .text, not a symtab.
                eb.add_rela(".rela.debug_info", t, d, {{0, 1, R_64, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // SHT_REL is rejected; unknown relocations against a different,
        // unrequested section do not block this one (isolation).
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                unsigned ln = eb.add(".debug_line", SHT_PROGBITS_, 0,
                                     vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rel.debug_info", sm, d, {{0, 1, R_64, 0}},
                            SHT_REL_);
                eb.add_rela(".rela.debug_line.weird", sm, ln,
                            {{0, 1, 777, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm;
                bm[d] = 0; bm[ln] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                unsigned ln = eb.add(".debug_line", SHT_PROGBITS_, 0,
                                     vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_line.weird", sm, ln,
                            {{0, 1, 777, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm;
                bm[d] = 0; bm[ln] = 0; bm[t] = 0x1000;
                auto l = make_loader(mm, bm);
                size_t s = 0;
                TASK_CHECK(l->load(dwarf::section_type::info, &s) != nullptr);
                TASK_CHECK(s == 16);
                TASK_CHECK(throws([&]() {
                        size_t s2;
                        l->load(dwarf::section_type::line, &s2);
                }));
        }

        // Compressed debug section is rejected.
        {
                elf_builder eb;
                eb.add(".debug_info", SHT_PROGBITS_, SHF_COMPRESSED_,
                       vector<unsigned char>(16, 0));
                auto mm = eb.build();
                auto l = make_loader(mm, map<unsigned, uint64_t>{});
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // Unsupported ELF flavors are refused at creation.
        {
                elf_builder eb;
                eb.machine = 0; // not EM_X86_64
                eb.add(".debug_info", SHT_PROGBITS_, 0,
                       vector<unsigned char>(8, 0));
                auto mm = eb.build();
                TASK_CHECK(throws([&]() {
                        make_loader(mm, map<unsigned, uint64_t>{});
                }));
        }
        {
                elf_builder eb;
                eb.type = 2; // ET_EXEC, not ET_REL
                eb.add(".debug_info", SHT_PROGBITS_, 0,
                       vector<unsigned char>(8, 0));
                auto mm = eb.build();
                TASK_CHECK(throws([&]() {
                        make_loader(mm, map<unsigned, uint64_t>{});
                }));
        }
        {
                elf_builder eb;
                eb.ei_class = 1; // ELF32
                eb.add(".debug_info", SHT_PROGBITS_, 0,
                       vector<unsigned char>(8, 0));
                auto mm = eb.build();
                TASK_CHECK(throws([&]() {
                        make_loader(mm, map<unsigned, uint64_t>{});
                }));
        }

        // -------- Boundary fixtures for each computation --------

        // Helper: one ABS symbol with a given value, one rela entry.
        auto abs_fixture = [](uint32_t rtype, uint64_t absval, int64_t addend,
                              uint64_t dbg_base = 0, uint64_t roff = 0) {
                elf_builder eb;
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(32, 0));
                strtab_builder sb;
                uint32_t nm = sb.add("a");
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {
                        {0, 0, 0, 0, 0},
                        {nm, sym_info(1, 0), 0xfff1, absval, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d,
                            {{roff, 1, rtype, addend}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = dbg_base;
                return make_pair(mm, bm);
        };

        // R_X86_64_64 boundaries.
        {
                auto fx = abs_fixture(R_64, 0xFFFFFFFFFFFFFFFFull, 0);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK(get64(q) == UINT64_MAX);
        }
        {
                auto fx = abs_fixture(R_64, 0xFFFFFFFFFFFFFFFFull, 1);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                auto fx = abs_fixture(R_64, 0, -1);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // R_X86_64_32 (unsigned) boundaries, including an intermediate
        // value above 32 bits that comes back into range.
        {
                auto fx = abs_fixture(R_32, 0xFFFFFFFF, 0);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK(get32(q) == UINT32_MAX);
        }
        {
                auto fx = abs_fixture(R_32, 0x100000000ull, -1);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK(get32(q) == UINT32_MAX);
        }
        {
                auto fx = abs_fixture(R_32, 0x100000000ull, 0);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                auto fx = abs_fixture(R_32, 0, -1);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // R_X86_64_32S (signed) boundaries.
        {
                auto fx = abs_fixture(R_32S, 0x7FFFFFFF, 0);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK((int32_t)get32(q) == INT32_MAX);
        }
        {
                auto fx = abs_fixture(R_32S, 0, INT32_MIN);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK((int32_t)get32(q) == INT32_MIN);
        }
        {
                // Positive 2^31 cannot be represented in signed int32.
                auto fx = abs_fixture(R_32S, 0x80000000, 0);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                // 2^31 overflows int32 by itself, minus one fits.
                auto fx = abs_fixture(R_32S, 0x80000000, -1);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK((int32_t)get32(q) == INT32_MAX);
        }
        {
                auto fx = abs_fixture(R_32S, 0x80000000, 1);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                // Negative overflow: sv = INT32_MIN - 1.
                auto fx = abs_fixture(R_32S, 0, (int64_t)INT32_MIN - 1);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // R_X86_64_PC32 boundaries: r_offset 0, P = dbg_base = 0x1000.
        {
                // S + A - P = INT32_MAX
                auto fx = abs_fixture(
                        R_PC32, 0, (int64_t)INT32_MAX + 0x1000, 0x1000, 0);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK((int32_t)get32(q) == INT32_MAX);
        }
        {
                // S + A - P = INT32_MIN
                auto fx = abs_fixture(
                        R_PC32, 0, (int64_t)INT32_MIN + 0x1000, 0x1000, 0);
                auto l = make_loader(fx.first, fx.second);
                size_t s; const unsigned char *q =
                        (const unsigned char *)l->load(
                                dwarf::section_type::info, &s);
                TASK_CHECK((int32_t)get32(q) == INT32_MIN);
        }
        {
                // One past the positive limit.
                auto fx = abs_fixture(
                        R_PC32, 0, (int64_t)INT32_MAX + 0x1000 + 1,
                        0x1000, 0);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }
        {
                // One past the negative limit.
                auto fx = abs_fixture(
                        R_PC32, 0, (int64_t)INT32_MIN + 0x1000 - 1,
                        0x1000, 0);
                auto l = make_loader(fx.first, fx.second);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // Two tables on one target: adjacent writes succeed, overlapping
        // writes fail.
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info.a", sm, d, {{0, 1, R_32, 0}});
                eb.add_rela(".rela.debug_info.b", sm, d, {{4, 1, R_32, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x2000;
                auto l = make_loader(mm, bm);
                size_t s;
                TASK_CHECK(l->load(dwarf::section_type::info, &s) != nullptr);
        }
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info.a", sm, d, {{0, 1, R_64, 0}});
                eb.add_rela(".rela.debug_info.b", sm, d, {{4, 1, R_32, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[d] = 0; bm[t] = 0x2000;
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // Base table keys are physical indices: a bogus index is refused.
        {
                elf_builder eb;
                eb.add(".debug_info", SHT_PROGBITS_, 0,
                       vector<unsigned char>(8, 0));
                auto mm = eb.build();
                map<unsigned, uint64_t> bm; bm[999] = 0;
                TASK_CHECK(throws([&]() { make_loader(mm, bm); }));
        }

        // S = base + st_value must not wrap around.
        {
                elf_builder eb;
                unsigned t = eb.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0));
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {0, sym_info(0, 3), (uint16_t)t,
                                       0x10, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d, {{0, 1, R_64, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm;
                bm[d] = 0;
                bm[t] = UINT64_MAX - 0xF; // + st_value 0x10 wraps
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        // P = target base + r_offset must not wrap around.
        {
                elf_builder eb;
                unsigned d = eb.add(".debug_info", SHT_PROGBITS_, 0,
                                    vector<unsigned char>(16, 0));
                strtab_builder sb;
                uint32_t nm = sb.add("a");
                unsigned st = eb.add_strtab(".strtab", sb.bytes);
                vector<sym_def> sy = {{0, 0, 0, 0, 0},
                                      {nm, sym_info(1, 0), 0xfff1, 0, 0}};
                unsigned sm = eb.add_symtab(".symtab", st, sy);
                eb.add_rela(".rela.debug_info", sm, d,
                            {{8, 1, R_PC32, 0}});
                auto mm = eb.build();
                map<unsigned, uint64_t> bm;
                bm[d] = UINT64_MAX; // + r_offset 8 wraps
                auto l = make_loader(mm, bm);
                TASK_CHECK(throws([&]() {
                        size_t s; l->load(dwarf::section_type::info, &s);
                }));
        }

        return 1;
}

// =================================================================
// contract_dwarf: relocated low_pc, a relocated cross-DIE reference
// and relocated strp offsets must flow through the real DWARF
// parser and yield a correct compilation unit.
// =================================================================

int
contract_dwarf()
{
        elf_builder b;

        // ---- .debug_abbrev (21 meaningful bytes + padding) ----
        vector<unsigned char> abbrev = {
                1, 0x11, 0x00,             // code 1, CU, no children
                0x03, 0x0e,               // DW_AT_name, DW_FORM_strp
                0x11, 0x01,               // DW_AT_low_pc, DW_FORM_addr
                0x47, 0x10,               // DW_AT_specification, ref_addr
                0, 0,                     // attr terminator
                2, 0x11, 0x00,             // code 2, CU, no children
                0x03, 0x0e,
                0x11, 0x01,
                0, 0,
                0                         // table terminator
        };
        while (abbrev.size() < 25)
                abbrev.push_back(0);
        const unsigned ABB = b.add(".debug_abbrev", SHT_PROGBITS_, 0, abbrev);

        // ---- .debug_str ----
        vector<unsigned char> strsec;
        strsec.push_back(0);                        // offset 0
        const char *s0 = "relocated_cu";
        uint32_t off_s0 = (uint32_t)strsec.size();  // 1
        strsec.insert(strsec.end(), s0, s0 + strlen(s0) + 1);
        const char *s1 = "target_die";
        uint32_t off_s1 = (uint32_t)strsec.size();  // 15
        strsec.insert(strsec.end(), s1, s1 + strlen(s1) + 1);
        while (strsec.size() < 32)
                strsec.push_back(0);
        const unsigned STRD = b.add(".debug_str", SHT_PROGBITS_, 0, strsec);

        // ---- .debug_info: two CUs, placeholders all zero ----
        // CU0: header(11) + code(1) + strp(4) + addr(8) + ref(8) = 32
        // CU1 begins at offset 32; its root DIE begins at offset 43.
        vector<unsigned char> info(56, 0);
        put32(info, 0, 28);                 // CU0 unit_length
        put16(info, 4, 4);                  // version
        put32(info, 6, 0);                  // abbrev offset
        info[10] = 8;                       // address size
        info[11] = 1;                       // abbrev code 1
        // strp @12, low_pc @16, ref_addr @24: relocated fields
        put32(info, 32, 20);                // CU1 unit_length
        put16(info, 36, 4);
        put32(info, 38, 0);
        info[42] = 8;
        info[43] = 2;                       // abbrev code 2
        // strp @44, low_pc @48
        const unsigned INFO = b.add(".debug_info", SHT_PROGBITS_, 0, info);

        const unsigned TEXT = b.add(".text", SHT_PROGBITS_,
                                    SHF_ALLOC_ | SHF_EXECINSTR_,
                                    vector<unsigned char>(128, 0), 0, 8);

        strtab_builder symstr;
        uint32_t n_func = symstr.add("myfunc");
        const unsigned SYMSTR = b.add_strtab(".strtab", symstr.bytes);
        const vector<sym_def> syms = {
                {0, 0, 0, 0, 0},
                {0, sym_info(0, 3), (uint16_t)TEXT, 0, 0},          // 1 section .text
                {n_func, sym_info(1, 2), (uint16_t)TEXT, 0x40, 0}, // 2 myfunc
                {0, sym_info(0, 3), (uint16_t)INFO, 0, 0},         // 3 section .debug_info
                {0, sym_info(0, 3), (uint16_t)STRD, 0, 0},         // 4 section .debug_str
                {0, sym_info(0, 3), (uint16_t)ABB, 0, 0},          // 5 section .debug_abbrev
        };
        const unsigned SYM = b.add_symtab(".symtab", SYMSTR, syms);

        // Non-conventional table names on purpose.
        b.add_rela(".rela.debug_info.table_a", SYM, INFO, {
                {12, 4, R_32,   (int64_t)off_s0},  // CU0 name strp
                {16, 2, R_64,   8},                // CU0 low_pc = base+0x48
                {24, 3, R_64,   43},               // spec -> CU1 root DIE
                {44, 4, R_32,   (int64_t)off_s1},  // CU1 name strp
                {48, 2, R_64,   0},                // CU1 low_pc = base+0x40
        });
        // Real relocations on the other two debug sections, placed in
        // tail padding so parsing is unaffected; they still force base
        // lookups for every debug section.
        b.add_rela(".rela.debug_abbrev.table", SYM, ABB,
                   {{21, 5, R_32, 0}});
        b.add_rela(".rela.debug_str.table", SYM, STRD,
                   {{26, 4, R_32, 0}});

        auto mem = b.build();
        const uint64_t text_base = 0x400000;
        map<unsigned, uint64_t> bases;
        bases[TEXT] = text_base;
        bases[INFO] = 0;
        bases[ABB] = 0;
        bases[STRD] = 0;

        // The plain loader keeps seeing unresolved placeholders.
        {
                elf::elf f(mem);
                dwarf::dwarf dbg(dwarf::elf::create_loader(f));
                TASK_CHECK(dbg.compilation_units().size() == 2);
                const auto &root0 = dbg.compilation_units()[0].root();
                TASK_CHECK(root0[dwarf::DW_AT::low_pc].as_address() == 0);
                TASK_CHECK(root0[dwarf::DW_AT::name].as_string() == "");
        }

        // The relocated loader feeds resolved data to the DWARF parser.
        dwarf::dwarf dbg;
        {
                elf::elf f(mem);
                auto l = dwarf::elf::create_relocated_loader(f, bases);
                dbg = dwarf::dwarf(l);
        }
        TASK_CHECK(dbg.compilation_units().size() == 2);

        const dwarf::die &r0 = dbg.compilation_units()[0].root();
        TASK_CHECK(r0[dwarf::DW_AT::name].as_string() == "relocated_cu");
        TASK_CHECK(r0[dwarf::DW_AT::low_pc].as_address() ==
                   text_base + 0x48);

        dwarf::die target = r0[dwarf::DW_AT::specification].as_reference();
        TASK_CHECK(target.valid());
        TASK_CHECK(target[dwarf::DW_AT::name].as_string() == "target_die");
        TASK_CHECK(target[dwarf::DW_AT::low_pc].as_address() ==
                   text_base + 0x40);
        // The reference lands in the second compilation unit.
        TASK_CHECK(&target.get_unit() ==
                   &dbg.compilation_units()[1].root().get_unit());

        const dwarf::die &r1 = dbg.compilation_units()[1].root();
        TASK_CHECK(r1[dwarf::DW_AT::name].as_string() == "target_die");
        TASK_CHECK(r1[dwarf::DW_AT::low_pc].as_address() ==
                   text_base + 0x40);

        // Original image remains untouched.
        elf::elf raw(mem);
        const unsigned char *raw_info =
                (const unsigned char *)raw.get_section(INFO).data();
        TASK_CHECK(get64(raw_info + 16) == 0);
        TASK_CHECK(get32(raw_info + 12) == 0);

        // A second, differently based view gives different addresses and
        // must not share state with the first.
        {
                map<unsigned, uint64_t> b2 = bases;
                b2[TEXT] = 0x7fff0000ull;
                elf::elf f(mem);
                dwarf::dwarf other(
                        dwarf::elf::create_relocated_loader(f, b2));
                TASK_CHECK(other.compilation_units()[0].root()
                           [dwarf::DW_AT::low_pc].as_address() ==
                           0x7fff0048ull);
                // First view is unaffected.
                TASK_CHECK(dbg.compilation_units()[0].root()
                           [dwarf::DW_AT::low_pc].as_address() ==
                           text_base + 0x48);
        }

        return 1;
}

// =================================================================
// contract_ownership: lifetime, stability, copied views and base
// table isolation.
// =================================================================

int
contract_ownership()
{
        elf_builder b;
        const unsigned TEXT = b.add(".text", SHT_PROGBITS_, SHF_ALLOC_,
                                    vector<unsigned char>(16, 0), 0, 8);
        vector<unsigned char> info(24, 0);
        vector<unsigned char> abb(8, 0);
        const unsigned INFO = b.add(".debug_info", SHT_PROGBITS_, 0, info);
        const unsigned ABB = b.add(".debug_abbrev", SHT_PROGBITS_, 0, abb);
        strtab_builder sb;
        const unsigned ST = b.add_strtab(".strtab", sb.bytes);
        const vector<sym_def> syms = {
                {0, 0, 0, 0, 0},
                {0, sym_info(0, 3), (uint16_t)TEXT, 0x30, 0}};
        const unsigned SYM = b.add_symtab(".symtab", ST, syms);
        b.add_rela(".rela.debug_info.x", SYM, INFO,
                   {{0, 1, R_64, 1}});

        auto mem = b.build();
        map<unsigned, uint64_t> bases;
        bases[TEXT] = 0x5000;
        bases[INFO] = 0;
        bases[ABB] = 0;

        // The view outlives every external object handed to the factory.
        shared_ptr<dwarf::loader> view;
        const unsigned char *info_ptr = nullptr;
        const unsigned char *abb_ptr = nullptr;
        size_t info_size = 0, abb_size = 0;
        {
                elf::elf f(mem);
                map<unsigned, uint64_t> local_bases = bases;
                view = dwarf::elf::create_relocated_loader(f, local_bases);
                info_ptr = (const unsigned char *)view->load(
                        dwarf::section_type::info, &info_size);
                abb_ptr = (const unsigned char *)view->load(
                        dwarf::section_type::abbrev, &abb_size);
        }
        // f, local_bases are gone; mem still exists here but will be
        // reset below before the final checks.
        TASK_CHECK(info_ptr != nullptr && abb_ptr != nullptr);
        TASK_CHECK(info_size == 24 && abb_size == 8);
        TASK_CHECK(get64(info_ptr) == 0x5031);

        // Repeated loads are stable; loading another section does not
        // invalidate earlier pointers.
        size_t info_size2 = 0;
        const unsigned char *info_ptr2 = (const unsigned char *)view->load(
                dwarf::section_type::info, &info_size2);
        TASK_CHECK(info_ptr2 == info_ptr && info_size2 == info_size);
        TASK_CHECK(get64(info_ptr) == 0x5031);
        TASK_CHECK(get64(abb_ptr) == 0);

        // A missing section is reported as nullptr, repeatedly.
        {
                size_t s = 123;
                TASK_CHECK(view->load(dwarf::section_type::line, &s) ==
                           nullptr);
                TASK_CHECK(s == 0);
                size_t s2 = 456;
                TASK_CHECK(view->load(dwarf::section_type::line, &s2) ==
                           nullptr);
                TASK_CHECK(s2 == 0);
        }

        // Now drop the source ELF storage itself; the view keeps its own
        // relocated copy (and keeps the backing loader alive).
        mem.reset();
        TASK_CHECK(get64(info_ptr) == 0x5031);
        const unsigned char *info_ptr3 = nullptr;
        size_t info_size3 = 0;
        info_ptr3 = (const unsigned char *)view->load(
                dwarf::section_type::info, &info_size3);
        TASK_CHECK(info_ptr3 == info_ptr && info_size3 == 24);

        // Copies of the view share storage and stay valid after the
        // original handle is dropped.
        shared_ptr<dwarf::loader> copy = view;
        view.reset();
        size_t cs = 0;
        const unsigned char *cp =
                (const unsigned char *)copy->load(
                        dwarf::section_type::info, &cs);
        TASK_CHECK(cp == info_ptr && cs == 24);
        TASK_CHECK(get64(cp) == 0x5031);

        // Mutating the caller's base table after creation has no effect.
        {
                auto mem2 = b.build();
                elf::elf f(mem2);
                auto l = dwarf::elf::create_relocated_loader(f, bases);
                bases[TEXT] = 0x9999;
                size_t s;
                const unsigned char *p = (const unsigned char *)l->load(
                        dwarf::section_type::info, &s);
                TASK_CHECK(get64(p) == 0x5031);
        }

        // Two views on the same object with different bases never share
        // cached bytes, yet each is internally stable.
        {
                auto mem3 = b.build();
                elf::elf f(mem3);
                map<unsigned, uint64_t> ba = bases; ba[TEXT] = 0x1000;
                map<unsigned, uint64_t> bb = bases; bb[TEXT] = 0x2000;
                auto la = dwarf::elf::create_relocated_loader(f, ba);
                auto lb = dwarf::elf::create_relocated_loader(f, bb);
                size_t sa, sb;
                const unsigned char *pa = (const unsigned char *)la->load(
                        dwarf::section_type::info, &sa);
                const unsigned char *pb = (const unsigned char *)lb->load(
                        dwarf::section_type::info, &sb);
                TASK_CHECK(pa != pb);
                TASK_CHECK(get64(pa) == 0x1031);
                TASK_CHECK(get64(pb) == 0x2031);
                const unsigned char *pa2 = (const unsigned char *)la->load(
                        dwarf::section_type::info, &sa);
                const unsigned char *pb2 = (const unsigned char *)lb->load(
                        dwarf::section_type::info, &sb);
                TASK_CHECK(pa2 == pa && pb2 == pb);
                TASK_CHECK(get64(pa) == 0x1031 && get64(pb) == 0x2031);
        }

        return 1;
}
