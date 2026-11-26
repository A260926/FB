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

#include <iostream>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <fcntl.h>    /* O_RDONLY */
#include <sys/stat.h> /* For the size of the file. , fstat */
#include <sys/mman.h> /* mmap, MAP_PRIVATE */
#include <vector>
#include <elf.h>      /* Elf32/Elf64 format declarations */

namespace elf_parser {

typedef struct {
    uint32_t section_index = 0;
    uint64_t section_offset = 0, section_addr = 0;
    std::string section_name;
    std::string section_type;
    uint64_t section_size = 0, section_ent_size = 0, section_addr_align = 0;
    // Raw sh_type / sh_link / sh_info / sh_flags, independent of file class.
    uint32_t section_type_id = 0, section_link = 0, section_info = 0;
    uint64_t section_flags = 0;
} section_t;

typedef struct {
    std::string segment_type, segment_flags;
    uint64_t segment_offset = 0, segment_virtaddr = 0, segment_physaddr = 0;
    uint64_t segment_filesize = 0, segment_memsize = 0;
    uint64_t segment_align = 0;
} segment_t;

typedef struct {
    std::string symbol_index;
    uint64_t symbol_value = 0;
    uint32_t symbol_num = 0;
    uint64_t symbol_size = 0;
    std::string symbol_type, symbol_bind, symbol_visibility, symbol_name, symbol_section;
    // Physical section index of the symbol table holding this record.
    uint32_t symbol_table_index = 0;
} symbol_t;

typedef struct {
    uint64_t relocation_offset = 0, relocation_info = 0, relocation_symbol_value = 0;
    std::string   relocation_type, relocation_symbol_name, relocation_section_name;
    uint64_t relocation_plt_address = 0;
    uint32_t relocation_type_id = 0;
    uint32_t relocation_symbol_table_index = 0, relocation_symbol_index = 0;
    uint32_t relocation_target_section_index = 0;
    bool relocation_has_addend = false;
    int64_t relocation_addend = 0;
} relocation_t;


class Elf_parser {
    public:
        Elf_parser (std::string &program_path): m_program_path{program_path} {
            load_memory_map();
        }
        std::vector<section_t> get_sections();
        std::vector<segment_t> get_segments();
        std::vector<symbol_t> get_symbols();
        std::vector<relocation_t> get_relocations();
        uint8_t *get_memory_map();

        // Values recorded in e_ident / e_machine; decoding follows the
        // file's own ELFCLASS and ELFDATA rather than the host layout.
        uint8_t elf_class() const { return m_class; }
        uint8_t data_encoding() const { return m_data; }
        uint16_t machine() const { return m_machine; }

    private:
        void load_memory_map();

        std::string get_section_type(uint32_t tt);

        std::string get_segment_type(uint32_t seg_type);
        std::string get_segment_flags(uint32_t seg_flags);

        std::string get_symbol_type(uint8_t sym_type);
        std::string get_symbol_bind(uint8_t sym_bind);
        std::string get_symbol_visibility(uint8_t sym_vis);
        std::string get_symbol_index(uint16_t sym_idx);

        std::string get_relocation_type(uint32_t rela_type);

        std::string m_program_path;
        uint8_t *m_mmap_program = nullptr;
        size_t m_file_size = 0;
        uint8_t m_class = 0;    /* ELFCLASS32 / ELFCLASS64 */
        uint8_t m_data = 0;     /* ELFDATA2LSB / ELFDATA2MSB */
        uint16_t m_machine = 0; /* EM_386 / EM_PPC / EM_X86_64 / EM_PPC64 */
};

}
#endif
