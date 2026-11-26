#include <iostream>
#include <inttypes.h> // PRIx64
#include "../elf_parser.hpp"

static const char *class_name(uint8_t elf_class) {
    switch (elf_class) {
        case ELFCLASS32: return "ELFCLASS32";
        case ELFCLASS64: return "ELFCLASS64";
        default:         return "UNKNOWN";
    }
}

static const char *encoding_name(uint8_t data_encoding) {
    switch (data_encoding) {
        case ELFDATA2LSB: return "ELFDATA2LSB (little endian)";
        case ELFDATA2MSB: return "ELFDATA2MSB (big endian)";
        default:          return "UNKNOWN";
    }
}

static const char *machine_name(uint16_t machine) {
    switch (machine) {
        case EM_386:    return "EM_386";
        case EM_PPC:    return "EM_PPC";
        case EM_X86_64: return "EM_X86_64";
        case EM_PPC64:  return "EM_PPC64";
        default:        return "UNKNOWN";
    }
}

void print_sections(std::vector<elf_parser::section_t> &sections);

int main(int argc, char* argv[]) {
    char usage_banner[] = "usage: ./sections [<executable>]\n";
    if(argc < 2) {
        std::cerr << usage_banner;
        return -1;
    }

    std::string program((std::string)argv[1]);
    elf_parser::Elf_parser elf_parser(program);

    // Metadata is read from e_ident/e_machine and follows the file's own
    // class and byte order, not the host running the parser.
    printf("Class:    %s\n", class_name(elf_parser.elf_class()));
    printf("Data:     %s\n", encoding_name(elf_parser.data_encoding()));
    printf("Machine:  %s\n", machine_name(elf_parser.machine()));

    std::vector<elf_parser::section_t> secs = elf_parser.get_sections();
    print_sections(secs);
    return 0;
}

void print_sections(std::vector<elf_parser::section_t> &sections) {
    printf("  [Nr] %-16s %-16s %-16s %s\n", "Name", "Type", "Address", "Offset");
    printf("       %-16s %-16s %5s  %s\n",
                    "Size", "EntSize", "Align", "Link/Info/Flags(raw)");

    for (auto &section : sections) {
        // Every address, offset, size, entsize and alignment is a uint64_t,
        // so 64-bit values are never truncated on a 32-bit-sized host field.
        printf("  [%2" PRIu32 "] %-16s %-16s %016" PRIx64 " %016" PRIx64 "\n",
            section.section_index,
            section.section_name.c_str(),
            section.section_type.c_str(),
            section.section_addr,
            section.section_offset);

        printf("       %016" PRIx64 " %016" PRIx64 " %5" PRIu64
               "  link=%" PRIu32 " info=%" PRIu32 " type=%" PRIu32
               " flags=0x%" PRIx64 "\n",
            section.section_size,
            section.section_ent_size,
            section.section_addr_align,
            section.section_link,
            section.section_info,
            section.section_type_id,
            section.section_flags);
    }
}
