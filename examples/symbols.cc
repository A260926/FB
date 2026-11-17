#include <cinttypes>
#include <cstdio>
#include <iostream>
#include <string>

#include "../elf_parser.hpp"
#include "meta.hh"

void print_symbols(std::vector<elf_parser::symbol_t> &symbols);

int main(int argc, char* argv[]) {
    char usage_banner[] = "usage: ./symbols [<executable>]\n";
    if(argc < 2) {
        std::cerr << usage_banner;
        return -1;
    }

    std::string program((std::string)argv[1]);
    elf_parser::Elf_parser elf_parser(program);

    print_elf_meta(elf_parser, argv[1]);

    std::vector<elf_parser::symbol_t> syms = elf_parser.get_symbols();
    print_symbols(syms);
    return 0;
}

void print_symbols(std::vector<elf_parser::symbol_t> &symbols) {
    printf("Num:    Value          Size     Type    Bind   Vis      Ndx Name\n");
    for (auto &symbol : symbols) {
        // symbol_num is the index within its own table; symbol_table_index
        // is the physical section index of that table.
        printf("%-3" PRIu32 ": %016" PRIx64 " %-8" PRIu64 " %-8s %-7s %-9s %-3s %s(%s#%" PRIu32 ")\n",
                symbol.symbol_num,
                symbol.symbol_value,
                symbol.symbol_size,
                symbol.symbol_type.c_str(),
                symbol.symbol_bind.c_str(),
                symbol.symbol_visibility.c_str(),
                symbol.symbol_index.c_str(),
                symbol.symbol_name.c_str(),
                symbol.symbol_section.c_str(),
                symbol.symbol_table_index);
    }
}
