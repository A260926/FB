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

#ifndef H_ELF_PARSER
#define H_ELF_PARSER

#include <cstdint>
#include <string>
#include <vector>

namespace elf_parser {

// All file offsets, addresses, raw relocation info, byte sizes, entry sizes
// and alignments are uint64_t so 64-bit targets never truncate; indices and
// counts are uint32_t.
struct section_t {
    uint32_t    section_index = 0;
    uint64_t    section_offset = 0, section_addr = 0;
    std::string section_name;
    std::string section_type;
    uint64_t    section_size = 0, section_ent_size = 0, section_addr_align = 0;
    // Raw sh_type / sh_link / sh_info / sh_flags of the section header.
    uint32_t    section_type_id = 0, section_link = 0, section_info = 0;
    uint64_t    section_flags = 0;
};

struct segment_t {
    std::string segment_type, segment_flags;
    uint64_t    segment_offset = 0, segment_virtaddr = 0, segment_physaddr = 0;
    uint64_t    segment_filesize = 0, segment_memsize = 0;
    uint64_t    segment_align = 0;
};

struct symbol_t {
    std::string symbol_index;
    uint64_t    symbol_value = 0;
    uint32_t    symbol_num = 0;         // index within its own table, null symbol included
    uint64_t    symbol_size = 0;
    uint32_t    symbol_table_index = 0; // physical section index of the containing table
    std::string symbol_type, symbol_bind, symbol_visibility, symbol_name, symbol_section;
};

struct relocation_t {
    uint64_t    relocation_offset = 0, relocation_info = 0, relocation_symbol_value = 0;
    std::string relocation_type, relocation_symbol_name, relocation_section_name;
    uint64_t    relocation_plt_address = 0; // display field, legacy .plt entry calculation
    uint32_t    relocation_type_id = 0;
    // sh_link / sh_info of the relocation section and the symbol index from r_info.
    uint32_t    relocation_symbol_table_index = 0, relocation_symbol_index = 0;
    uint32_t    relocation_target_section_index = 0;
    // RELA records carry an explicit signed addend; REL records do not.
    bool        relocation_has_addend = false;
    int64_t     relocation_addend = 0;
};

class Elf_parser {
    public:
        Elf_parser (std::string &program_path);
        ~Elf_parser();

        std::vector<section_t> get_sections();
        std::vector<segment_t> get_segments();
        std::vector<symbol_t> get_symbols();
        std::vector<relocation_t> get_relocations();
        uint8_t *get_memory_map();

        // File metadata as recorded in e_ident / e_machine.
        uint8_t  elf_class()     const { return m_elf_class; }     // ELFCLASS32 / ELFCLASS64
        uint8_t  data_encoding() const { return m_data_encoding; } // ELFDATA2LSB / ELFDATA2MSB
        uint16_t machine()       const { return m_machine; }       // EM_386 / EM_X86_64 / EM_PPC / EM_PPC64

    private:
        // Section header and symbol table entry decoded to host order; the
        // mapped bytes themselves are never reinterpreted as host structs.
        struct shdr_info {
            uint32_t name = 0, type = 0, link = 0, info = 0;
            uint64_t flags = 0, addr = 0, offset = 0, size = 0, addralign = 0, entsize = 0;
        };
        struct sym_info {
            uint32_t name = 0;
            uint64_t value = 0, size = 0;
            uint8_t  info = 0, other = 0;
            uint16_t shndx = 0;
        };

        void load_memory_map();
        void parse_image();

        uint16_t fetch16(uint64_t off) const;
        uint32_t fetch32(uint64_t off) const;
        uint64_t fetch64(uint64_t off) const;

        std::vector<sym_info> read_symbol_table(const shdr_info &tab) const;
        std::string read_string(const shdr_info &strtab, uint32_t off) const;
        std::string section_name(const shdr_info &sh) const;

        std::string get_section_type(uint32_t tt) const;

        std::string get_segment_type(uint32_t seg_type) const;
        std::string get_segment_flags(uint32_t seg_flags) const;

        std::string get_symbol_type(uint8_t sym_info) const;
        std::string get_symbol_bind(uint8_t sym_info) const;
        std::string get_symbol_visibility(uint8_t sym_other) const;
        std::string get_symbol_index(uint16_t sym_idx) const;

        std::string get_relocation_type(uint32_t type_id) const;

        std::string m_program_path;
        uint8_t    *m_mmap_program = nullptr;
        uint64_t    m_file_size = 0;
        uint8_t     m_elf_class = 0;
        uint8_t     m_data_encoding = 0;
        uint16_t    m_machine = 0;
        bool        m_is64 = false;
        bool        m_msb = false;
        uint64_t    m_phoff = 0, m_shoff = 0;
        uint32_t    m_phentsize = 0, m_phnum = 0;
        uint32_t    m_shentsize = 0, m_shnum = 0, m_shstrndx = 0;
        std::vector<shdr_info> m_shdrs;
};

}
#endif
