#include <iostream>
#include <inttypes.h> // PRIx64
#include "../elf_parser.hpp"

void print_relocations(std::vector<elf_parser::relocation_t> &relocations);

int main(int argc, char* argv[]) {
    char usage_banner[] = "usage: ./relocations [<executable>]\n";
    if(argc < 2) {
        std::cerr << usage_banner;
        return -1;
    }

    std::string program((std::string)argv[1]);
    elf_parser::Elf_parser elf_parser(program);

    std::vector<elf_parser::relocation_t> relocs = elf_parser.get_relocations();
    print_relocations(relocs);
    return 0;
}

void print_relocations(std::vector<elf_parser::relocation_t> &relocations) {
    printf("  [*] %-16s %-16s %-16s %-16s %s\n", "Offset", "Info", "Type", "Sym. Value", "Sym. Name (section)");
    printf("  [*] %-16s %-16s %-24s %s\n", "Type-Id", "Sym#/Table/Tgt", "Addend", "Calculated PLT Address");
    for (auto &rel : relocations) {
        printf("%016" PRIx64 "  %016" PRIx64 " %-16s %016" PRIx64 " %-16s (%s)\n",
            rel.relocation_offset,
            rel.relocation_info,
            rel.relocation_type.c_str(),
            rel.relocation_symbol_value,
            rel.relocation_symbol_name.c_str(),
            rel.relocation_section_name.c_str());

        // relocation_has_addend distinguishes RELA (explicit, sign-extended
        // addend) from REL, which has no addend in the file and must not be
        // guessed from the target's instruction bytes.
        char addend_buf[40];
        if (rel.relocation_has_addend)
            snprintf(addend_buf, sizeof(addend_buf), "%" PRId64, rel.relocation_addend);
        else
            snprintf(addend_buf, sizeof(addend_buf), "none (REL)");

        printf("  type=%" PRIu32 "       sym=%" PRIu32 " table=%" PRIu32 " tgt=%" PRIu32
               "  %-24s %016" PRIx64 "\n",
            rel.relocation_type_id,
            rel.relocation_symbol_index,
            rel.relocation_symbol_table_index,
            rel.relocation_target_section_index,
            addend_buf,
            rel.relocation_plt_address);
    }
}
