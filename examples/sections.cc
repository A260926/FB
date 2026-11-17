#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <string>

#include "../elf_parser.hpp"
#include "meta.hh"

void print_sections(std::vector<elf_parser::section_t> &sections);

int main(int argc, char* argv[]) {
    char usage_banner[] = "usage: ./sections [<executable>]\n";
    if(argc < 2) {
        std::cerr << usage_banner;
        return -1;
    }

    std::string program((std::string)argv[1]);
    elf_parser::Elf_parser elf_parser(program);

    print_elf_meta(elf_parser, argv[1]);

    std::vector<elf_parser::section_t> secs = elf_parser.get_sections();
    print_sections(secs);
    return 0;
}

void print_sections(std::vector<elf_parser::section_t> &sections) {
    printf("  [Nr] %-16s %-16s %-16s %s\n", "Name", "Type", "Address", "Offset");
    printf("       %-16s %-16s %5s %s\n",
                    "Size", "EntSize", "Align", "Flags/Link/Info");

    for (auto &section : sections) {
        printf("  [%2" PRIu32 "] %-16s %-16s %016" PRIx64 " %08" PRIx64 "\n",
            section.section_index,
            section.section_name.c_str(),
            section.section_type.c_str(),
            section.section_addr,
            section.section_offset);

        printf("       %016" PRIx64 " %016" PRIx64 " %5" PRIu64 " %" PRIx64 "/%" PRIu32 "/%" PRIu32 "\n",
            section.section_size,
            section.section_ent_size,
            section.section_addr_align,
            section.section_flags,
            section.section_link,
            section.section_info);
    }
}
