// Copyright (c) 2013 Austin T. Clements. All rights reserved.
// Use of this source code is governed by an MIT license
// that can be found in the LICENSE file.

#ifndef _DWARFPP_ELF_RELOC_HH_
#define _DWARFPP_ELF_RELOC_HH_

#include "dwarf++.hh"
#include "../elf/elf++.hh"

#include <cstdint>
#include <map>
#include <memory>

DWARFPP_BEGIN_NAMESPACE
namespace elf
{
        /**
         * Create a DWARF section loader that presents relocated views
         * of the debug sections of a relocatable ELF object.
         *
         * Unlike create_loader, this loader applies SHT_RELA
         * relocations before handing a section to the DWARF parser.
         * Relocation is purely an offline read: no code is loaded, the
         * underlying ELF image is never modified, and no system linker
         * is involved.
         *
         * Only ELF64, little-endian, EM_X86_64, ET_REL objects are
         * supported; any other combination is rejected with an
         * exception.  SHT_REL tables, compressed debug sections and
         * relocations that require external symbol resolution are
         * rejected when they affect a requested section.
         *
         * \param f
         *      The source ELF object.  The returned loader retains its
         *      own copy of this (reference counted) object, including
         *      everything needed to keep its backing storage alive.
         *
         * \param section_bases
         *      Base addresses keyed by *physical section index* (not
         *      section or symbol names).  The table is copied.  Every
         *      section that actually participates in computing a
         *      requested relocation must have an entry; a missing
         *      entry is an error rather than an implicit fallback to
         *      sh_addr.  Debug sections are conventionally given base
         *      0; allocatable sections such as .text may be assigned
         *      whatever address the caller arranged.
         *
         * Sections are relocated lazily on first load and the
         * relocated bytes are cached by the returned loader, so
         * repeated loads return stable addresses and contents.
         */
        std::shared_ptr<loader>
        create_relocated_loader(const ::elf::elf &f,
                                const std::map<unsigned, std::uint64_t> &section_bases);
}
DWARFPP_END_NAMESPACE

#endif
