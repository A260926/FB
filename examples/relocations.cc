#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <string>

#include "../elf_parser.hpp"
#include "meta.hh"

void print_relocations(std::vector<elf_parser::relocation_t> &relocations);

int main(int argc, char* argv[]) {
    char usage_banner[] = "usage: ./relocations [<executable>]\n";
    if(argc < 2) {
        std::cerr << usage_banner;
        return -1;
    }

    std::string program((std::string)argv[1]);
    elf_parser::Elf_parser elf_parser(program);

    print_elf_meta(elf_parser, argv[1]);

    std::vector<elf_parser::relocation_t> relocs = elf_parser.get_relocations();
    print_relocations(relocs);
    return 0;
}

void print_relocations(std::vector<elf_parser::relocation_t> &relocations) {
    printf("  [*] %-16s %-16s %-16s %-16s %s\n", "Offset", "Info", "Type", "Sym. Value", "Sym. Name (section)");
    printf("  [*] %-16s %-16s %s\n", "Addend", "Calc. PLT Address", "Sym#/SymTab#/Target#");
    for (auto &rel : relocations) {
        printf("%016" PRIx64 "  %016" PRIx64 " %-16s %016" PRIx64 " %-16s (%s)\n",
            rel.relocation_offset,
            rel.relocation_info,
            rel.relocation_type.c_str(),
            rel.relocation_symbol_value,
            rel.relocation_symbol_name.c_str(),
            rel.relocation_section_name.c_str());
        // relocation_has_addend tells RELA (explicit signed addend) from REL
        // (no addend at all); relocation_addend is meaningful only when
        // relocation_has_addend is true.
        if (rel.relocation_has_addend)
            printf("%16" PRId64 "  ", rel.relocation_addend);
        else
            printf("%16s  ", "-");
        printf("%016" PRIx64 " %" PRIu32 "/%" PRIu32 "/%" PRIu32 " type_id=%" PRIu32 "\n",
            rel.relocation_plt_address,
            rel.relocation_symbol_index,
            rel.relocation_symbol_table_index,
            rel.relocation_target_section_index,
            rel.relocation_type_id);
    }
}
