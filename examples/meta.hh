#pragma once

// Shared helper for the examples: print the file metadata the parser
// exposes from e_ident / e_machine before any table dump.

#include <cinttypes>
#include <cstdio>

#include "../elf_parser.hpp"

inline void print_elf_meta(elf_parser::Elf_parser &parser, const char *path) {
    const uint8_t cls = parser.elf_class();     // ELFCLASS32 / ELFCLASS64
    const uint8_t enc = parser.data_encoding(); // ELFDATA2LSB / ELFDATA2MSB
    std::printf("  [*] %s: %s, %s, e_machine=%" PRIu16 "\n", path,
        cls == 1 ? "ELF32" : cls == 2 ? "ELF64" : "ELF?",
        enc == 1 ? "LSB" : enc == 2 ? "MSB" : "?",
        parser.machine());
}
