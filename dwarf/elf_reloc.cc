// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#include "elf_reloc.hh"

#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

using namespace std;

DWARFPP_BEGIN_NAMESPACE
namespace elf
{
        namespace
        {
                // x86-64 relocation types (System V AMD64 ABI figure 4.9).
                enum R_X86_64
                {
                        R_X86_64_NONE = 0,
                        R_X86_64_64   = 1,
                        R_X86_64_32   = 10,
                        R_X86_64_32S  = 11,
                        R_X86_64_PC32 = 2,
                };

                static const unsigned EM_X86_64 = 62;
                static const unsigned SHN_LORESERVE = 0xff00;
                static const unsigned SHN_UNDEF_    = 0;
                static const unsigned SHN_ABS_      = 0xfff1;
                static const unsigned SHN_COMMON_   = 0xfff2;
                static const size_t RELA_ENT_SIZE  = 24; // Elf64_Rela
                static const size_t SYM_ENT_SIZE   = 24; // Elf64_Sym
                static const uint64_t SHF_COMPRESSED_ = 0x800;

                static uint32_t
                rd32(const unsigned char *p)
                {
                        return  (uint32_t)p[0]        | ((uint32_t)p[1] << 8) |
                                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                }

                static uint64_t
                rd64(const unsigned char *p)
                {
                        uint64_t v = 0;
                        for (unsigned i = 0; i < 8; i++)
                                v |= (uint64_t)p[i] << (8 * i);
                        return v;
                }

                static void
                wr32(vector<unsigned char> &b, size_t off, uint32_t v)
                {
                        for (unsigned i = 0; i < 4; i++)
                                b[off + i] = (unsigned char)(v >> (8 * i));
                }

                static void
                wr64(vector<unsigned char> &b, size_t off, uint64_t v)
                {
                        for (unsigned i = 0; i < 8; i++)
                                b[off + i] = (unsigned char)(v >> (8 * i));
                }

                class relocated_loader : public loader
                {
                public:
                        relocated_loader(const ::elf::elf &f,
                                         const map<unsigned, uint64_t> &bases)
                                : f(f), bases(bases)
                        {
                                const ::elf::Ehdr<> &h = f.get_hdr();
                                if (h.ei_class != ::elf::elfclass::_64)
                                        throw format_error("relocated loader: only ELF64 is supported (got ELFCLASS" +
                                                           std::to_string((unsigned)h.ei_class) + ")");
                                if (h.ei_data != ::elf::elfdata::lsb)
                                        throw format_error("relocated loader: only little-endian objects are supported (got ELFDATA " +
                                                           std::to_string((unsigned)h.ei_data) + ")");
                                if (h.machine != EM_X86_64)
                                        throw format_error("relocated loader: only EM_X86_64 is supported (got e_machine " +
                                                           std::to_string(h.machine) + ")");
                                if (h.type != ::elf::et::rel)
                                        throw format_error("relocated loader: only ET_REL objects are supported (got e_type " +
                                                           std::to_string((int)h.type) + ")");

                                unsigned shnum = f.sections().size();
                                for (auto &kv : bases) {
                                        if (kv.first >= shnum)
                                                throw format_error("relocated loader: base table entry for section " +
                                                                   std::to_string(kv.first) +
                                                                   " is not a valid section index");
                                }
                        }

                        const void *
                        load(section_type section, size_t *size_out) override
                        {
                                lock_guard<mutex> g(lock);

                                auto it = cache.find(section);
                                if (it != cache.end()) {
                                        if (!it->second) {
                                                *size_out = 0;
                                                return nullptr;
                                        }
                                        *size_out = it->second->size();
                                        return it->second->data();
                                }

                                auto bytes = make_shared<vector<unsigned char> >();
                                if (!build(section, *bytes)) {
                                        // The object has no such section.
                                        cache.insert(make_pair(section,
                                                               shared_ptr<vector<unsigned char> >()));
                                        *size_out = 0;
                                        return nullptr;
                                }
                                auto ins = cache.insert(make_pair(section, bytes));
                                *size_out = ins.first->second->size();
                                return ins.first->second->data();
                        }

                private:
                        ::elf::elf f;
                        map<unsigned, uint64_t> bases;
                        mutex lock;
                        // std::map nodes are stable, so loading another
                        // section can never invalidate earlier pointers.
                        // A null value records a section the object lacks.
                        map<section_type, shared_ptr<vector<unsigned char> > > cache;

                        uint64_t
                        require_base(unsigned secidx, const char *what)
                        {
                                auto it = bases.find(secidx);
                                if (it == bases.end())
                                        throw format_error(string("relocated loader: ") + what +
                                                           " requires a base address for section " +
                                                           std::to_string(secidx) +
                                                           ", but the base table has no entry for it");
                                return it->second;
                        }

                        // Locate the physical section holding the requested
                        // DWARF section type.  Returns -1 if the object has
                        // no such section.
                        int
                        find_section(section_type type)
                        {
                                const char *want = section_type_to_name(type);
                                const auto &secs = f.sections();
                                for (unsigned i = 1; i < secs.size(); i++) {
                                        if (!secs[i].valid())
                                                continue;
                                        if (secs[i].get_name() == want)
                                                return (int)i;
                                }
                                return -1;
                        }

                        // Resolve the S value of the symbol at symidx in the
                        // symbol table section symsec.  Diagnostics carry the
                        // relocation table and entry position.
                        uint64_t
                        resolve_symbol(unsigned symsec, unsigned symidx,
                                       unsigned relsec, unsigned entidx)
                        {
                                const ::elf::section &ss = f.get_section(symsec);
                                size_t symsize = ss.size();
                                const void *symdata = ss.data();
                                if (!symdata && symsize != 0)
                                        throw format_error("relocated loader: symbol table section " +
                                                           std::to_string(symsec) +
                                                           " has no data");
                                if ((uint64_t)symidx * SYM_ENT_SIZE + SYM_ENT_SIZE > symsize)
                                        throw format_error("relocated loader: relocation section " +
                                                           std::to_string(relsec) + " entry " + std::to_string(entidx) +
                                                           " references symbol " + std::to_string(symidx) +
                                                           " beyond the end of symbol table section " +
                                                           std::to_string(symsec));

                                const unsigned char *sp =
                                        (const unsigned char *)symdata + (uint64_t)symidx * SYM_ENT_SIZE;
                                uint32_t st_name  = rd32(sp + 0);
                                uint8_t  st_info  = sp[4];
                                uint16_t st_shndx = (uint16_t)rd32(sp + 6);
                                uint64_t st_value = rd64(sp + 8);
                                (void)st_name;
                                (void)st_info;

                                string loc = "relocation section " + std::to_string(relsec) +
                                             " entry " + std::to_string(entidx) + " symbol " +
                                             std::to_string(symidx);

                                if (st_shndx == SHN_UNDEF_)
                                        throw format_error("relocated loader: " + loc +
                                                           " is undefined and cannot be resolved offline");
                                if (st_shndx == SHN_COMMON_)
                                        throw format_error("relocated loader: " + loc +
                                                           " is COMMON and cannot be resolved offline");
                                if (st_shndx >= SHN_LORESERVE) {
                                        if (st_shndx == SHN_ABS_)
                                                return st_value;
                                        throw format_error("relocated loader: " + loc +
                                                           " uses reserved/extended section index " +
                                                           std::to_string(st_shndx) +
                                                           " which is not supported");
                                }

                                if (st_shndx >= f.sections().size())
                                        throw format_error("relocated loader: " + loc +
                                                           " references out-of-range section index " +
                                                           std::to_string(st_shndx));

                                uint64_t base = require_base(st_shndx, loc.c_str());
                                uint64_t s;
                                if (__builtin_add_overflow(base, st_value, &s))
                                        throw format_error("relocated loader: " + loc +
                                                           " S = base + st_value wraps around");
                                return s;
                        }

                        // Build the relocated view of one DWARF section.
                        // Returns false if the object has no such section.
                        bool
                        build(section_type type, vector<unsigned char> &out)
                        {
                                int target = find_section(type);
                                if (target < 0) {
                                        // Section does not exist in this object.
                                        return false;
                                }

                                const ::elf::section &ts = f.get_section((unsigned)target);
                                const ::elf::Shdr<> &th = ts.get_hdr();
                                string tname = ts.get_name();
                                if (((uint64_t)th.flags & SHF_COMPRESSED_) != 0 ||
                                    tname.compare(0, 8, ".zdebug") == 0)
                                        throw format_error("relocated loader: compressed debug section " +
                                                           tname + " is not supported");
                                if (th.type == ::elf::sht::nobits)
                                        throw format_error("relocated loader: debug section " + tname +
                                                           " cannot be SHT_NOBITS");

                                const auto &secs = f.sections();

                                // Reject SHT_REL tables affecting this section.
                                for (unsigned i = 1; i < secs.size(); i++) {
                                        const ::elf::section &rs = secs[i];
                                        if (!rs.valid())
                                                continue;
                                        if (rs.get_hdr().type == ::elf::sht::rel &&
                                            rs.get_hdr().info == (unsigned)target)
                                                throw format_error("relocated loader: SHT_REL section " +
                                                                   std::to_string(i) + " targeting " + tname +
                                                                   " is not supported (only SHT_RELA)");
                                }

                                // Copy the pristine section bytes.  The
                                // original ELF image is never touched.
                                size_t tsize = ts.size();
                                out.resize(tsize);
                                if (tsize)
                                        memcpy(out.data(), ts.data(), tsize);

                                // Per-byte owning relocation table (-1 if
                                // untouched).  A byte written by two distinct
                                // tables is an overlap; repeated writes from
                                // the same table are applied in order.
                                vector<int> owner(tsize, -1);

                                for (unsigned i = 1; i < secs.size(); i++) {
                                        const ::elf::section &rs = secs[i];
                                        if (!rs.valid())
                                            continue;
                                        const ::elf::Shdr<> &rh = rs.get_hdr();
                                        if (rh.type != ::elf::sht::rela || rh.info != (unsigned)target)
                                                continue;

                                        // sh_link points at this table's own
                                        // symbol table; sh_info at the target.
                                        if (rh.info >= secs.size() || !secs[rh.info].valid())
                                                throw format_error("relocated loader: relocation section " +
                                                                   std::to_string(i) + " has bad sh_info " +
                                                                   std::to_string(rh.info));
                                        unsigned symsec = rh.link;
                                        if (symsec >= secs.size() || !secs[symsec].valid())
                                                throw format_error("relocated loader: relocation section " +
                                                                   std::to_string(i) + " has bad sh_link " +
                                                                   std::to_string(symsec));
                                        const ::elf::section &symtabsec = secs[symsec];
                                        if (symtabsec.get_hdr().type != ::elf::sht::symtab &&
                                            symtabsec.get_hdr().type != ::elf::sht::dynsym)
                                                throw format_error("relocated loader: relocation section " +
                                                                   std::to_string(i) + " sh_link " +
                                                                   std::to_string(symsec) +
                                                                   " is not a symbol table");
                                        if (rh.entsize != 0 && rh.entsize != RELA_ENT_SIZE)
                                                throw format_error("relocated loader: relocation section " +
                                                                   std::to_string(i) + " has bad sh_entsize " +
                                                                   std::to_string((uint64_t)rh.entsize));
                                        size_t relasize = rs.size();
                                        if (relasize % RELA_ENT_SIZE != 0)
                                                throw format_error("relocated loader: relocation section " +
                                                                   std::to_string(i) +
                                                                   " has a truncated trailing entry");
                                        const unsigned char *rp =
                                                (const unsigned char *)rs.data();
                                        if (!rp && relasize != 0)
                                                throw format_error("relocated loader: relocation section " +
                                                                   std::to_string(i) + " has no data");

                                        unsigned nent = (unsigned)(relasize / RELA_ENT_SIZE);
                                        for (unsigned j = 0; j < nent; j++) {
                                                const unsigned char *ep = rp + (uint64_t)j * RELA_ENT_SIZE;
                                                uint64_t r_offset = rd64(ep + 0);
                                                uint64_t r_info   = rd64(ep + 8);
                                                int64_t  r_addend = (int64_t)rd64(ep + 16);
                                                unsigned rtype  = (unsigned)(r_info & 0xffffffff);
                                                unsigned rsym   = (unsigned)(r_info >> 32);

                                                string loc = "relocation section " + std::to_string(i) +
                                                             " entry " + std::to_string(j) +
                                                             " (target section " + std::to_string(target) +
                                                             " " + tname + " offset " +
                                                             std::to_string(r_offset) + ")";

                                                if (rtype == R_X86_64_NONE)
                                                        continue;

                                                unsigned field = 0;
                                                switch (rtype) {
                                                case R_X86_64_64:
                                                        field = 8;
                                                        break;
                                                case R_X86_64_32:
                                                case R_X86_64_32S:
                                                case R_X86_64_PC32:
                                                        field = 4;
                                                        break;
                                                default:
                                                        throw format_error("relocated loader: " + loc +
                                                                           " has unknown/unsupported R_X86_64 type " +
                                                                           std::to_string(rtype));
                                                }

                                                if (r_offset > tsize || field > tsize - r_offset)
                                                        throw format_error("relocated loader: " + loc +
                                                                           " writes past the end of its target section");

                                                // Non-NONE writes from two distinct
                                                // relocation tables may not share a
                                                // byte; adjacent ranges are fine.
                                                for (uint64_t k = r_offset;
                                                     k < r_offset + field; k++) {
                                                        if (owner[k] >= 0 &&
                                                            (unsigned)owner[k] != i)
                                                                throw format_error("relocated loader: overlapping relocations in section " +
                                                                                   tname + " at offset " +
                                                                                   std::to_string(k));
                                                }

                                                uint64_t S = resolve_symbol(symsec, rsym, i, j);

                                                // Signed 128-bit arithmetic: S and
                                                // P themselves never wrap, but the
                                                // intermediate sum may exceed the
                                                // narrow field before coming back
                                                // into range.
                                                __int128 sv = (__int128)S + (__int128)r_addend;

                                                if (rtype == R_X86_64_64) {
                                                        if (sv < 0 || sv > (__int128)UINT64_MAX)
                                                                throw format_error("relocated loader: " + loc +
                                                                                   " result is not representable in uint64");
                                                        wr64(out, (size_t)r_offset, (uint64_t)sv);
                                                } else if (rtype == R_X86_64_32) {
                                                        if (sv < 0 || sv > (__int128)UINT32_MAX)
                                                                throw format_error("relocated loader: " + loc +
                                                                                   " result is not representable in uint32");
                                                        wr32(out, (size_t)r_offset, (uint32_t)sv);
                                                } else if (rtype == R_X86_64_32S) {
                                                        if (sv < (__int128)INT32_MIN ||
                                                            sv > (__int128)INT32_MAX)
                                                                throw format_error("relocated loader: " + loc +
                                                                                   " result is not representable in int32");
                                                        wr32(out, (size_t)r_offset, (uint32_t)(int32_t)sv);
                                                } else {
                                                        uint64_t base =
                                                                require_base((unsigned)target, loc.c_str());
                                                        uint64_t P;
                                                        if (__builtin_add_overflow(base, r_offset, &P))
                                                                throw format_error("relocated loader: " + loc +
                                                                                   " P = base + r_offset wraps around");
                                                        __int128 rv = sv - (__int128)P;
                                                        if (rv < (__int128)INT32_MIN ||
                                                            rv > (__int128)INT32_MAX)
                                                                throw format_error("relocated loader: " + loc +
                                                                                   " PC32 result is not representable in int32");
                                                        wr32(out, (size_t)r_offset, (uint32_t)(int32_t)rv);
                                                }

                                                for (uint64_t k = r_offset;
                                                     k < r_offset + field; k++)
                                                        owner[k] = (int)i;
                                        }
                                }
                                return true;
                        }
                };
        }

        shared_ptr<loader>
        create_relocated_loader(const ::elf::elf &f,
                                const map<unsigned, uint64_t> &section_bases)
        {
                return make_shared<relocated_loader>(f, section_bases);
        }
}
DWARFPP_END_NAMESPACE
